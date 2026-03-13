/*
 * MDEC IPU — Hardware-accelerated MDEC decode using PS2 IPU
 *
 * Pre-dequant approach: MDEC RLE is parsed in software, the DC and AC
 * coefficients are dequantized using the exact MDEC formulas, then
 * re-encoded as MPEG-2 intra VLC bitstream.  The IPU receives this
 * bitstream via IDEC with an identity quantization table, so the IPU
 * effectively performs only IDCT + YCbCr→RGB conversion.
 *
 * Key design:
 *   - Identity quant table: W[k]=8 for all k
 *   - IDEC QSC=2 from command → quantizer_scale=2
 *   - AC dequant in IPU: (level * 2 * 8) / 16 = level  (identity!)
 *   - IDP=2 in IPU_CTRL (10-bit DC precision, pred=512)
 *   - DC: F[0] = (512 + dc_diff) << 1 = (512 + dc_diff) * 2
 *   - BitWriter byte-swaps for big-endian bitstream (IPU convention)
 *
 * Reference: PCSX2 IPU_MultiISA.cpp, ps2sdk libmpeg_core.c
 */

#ifdef ENABLE_MDEC_IPU

#include "mdec.h"
#include "superpsx.h"
#include <string.h>
#include <stdio.h>
#include <ee_regs.h>
#include <kernel.h>

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "MDEC-IPU"

/* ── IPU command opcodes ─────────────────────────────────────────── */
#define IPU_CMD_BCLR   0x00000000
#define IPU_CMD_IDEC   0x10000000
#define IPU_CMD_SETIQ  0x50000000

#define IPU_CTRL_BUSY  (1u << 31)
#define IPU_CTRL_RST   (1u << 30)

/* IDP field in IPU_CTRL: bits 17:16 (per PCSX2 tIPU_CTRL / EE Users Manual) */
#define IPU_CTRL_IDP_SHIFT 16
#define IPU_CTRL_IDP_10BIT (2u << IPU_CTRL_IDP_SHIFT)  /* 10-bit DC prec */

/* ── RLE helpers (same as mdec.c) ────────────────────────────────── */
#define RLE_RUN(hw)  ((hw) >> 10)
#define RLE_VAL(hw)  (((int)(hw) << (sizeof(int)*8 - 10)) >> (sizeof(int)*8 - 10))
#define MDEC_END_OF_DATA 0xFE00

/* ── Bitstream writer (big-endian output for IPU) ────────────────── */
typedef struct {
    uint32_t *buf;
    uint32_t  accum;    /* accumulator, MSB = first bit */
    int       bits;     /* free bits remaining in accum  */
    int       words;    /* 32-bit words written          */
} BitWriter;

static inline void bw_init(BitWriter *bw, uint32_t *buf) {
    bw->buf   = buf;
    bw->accum = 0;
    bw->bits  = 32;
    bw->words = 0;
}

/*
 * Emit 'nbits' bits from the MSB of 'code'.
 * E.g. bw_put(bw, 0x80000000, 1) emits a '1' bit.
 */
static inline void bw_put(BitWriter *bw, uint32_t code, int nbits) {
    uint32_t val = code >> (32 - nbits);   /* right-justify the bits */
    bw->bits -= nbits;
    if (bw->bits >= 0) {
        bw->accum |= val << bw->bits;
    } else {
        int overflow = -bw->bits;
        bw->accum |= val >> overflow;
        /* Byte-swap for big-endian IPU bitstream */
        bw->buf[bw->words++] = __builtin_bswap32(bw->accum);
        bw->accum = 0;
        bw->bits = 32 - overflow;
        if (overflow > 0)
            bw->accum = val << bw->bits;
    }
}

static inline void bw_flush(BitWriter *bw) {
    if (bw->bits < 32) {
        bw->buf[bw->words++] = __builtin_bswap32(bw->accum);
        bw->accum = 0;
        bw->bits = 32;
    }
}

/* ── MPEG-2 Intra DC encoding (Table B.12 luma, B.13 chroma) ──── */

/* Luminance DC size table (Table B.12) — {code, len} */
static const struct { uint16_t code; uint8_t len; } dc_lum[12] = {
    { 0x04, 3 }, /* size 0: 100 */
    { 0x00, 2 }, /* size 1: 00 */
    { 0x01, 2 }, /* size 2: 01 */
    { 0x05, 3 }, /* size 3: 101 */
    { 0x06, 3 }, /* size 4: 110 */
    { 0x0E, 4 }, /* size 5: 1110 */
    { 0x1E, 5 }, /* size 6: 11110 */
    { 0x3E, 6 }, /* size 7: 111110 */
    { 0x7E, 7 }, /* size 8: 1111110 */
    { 0xFE, 8 }, /* size 9: 11111110 */
    {0x1FE, 9 }, /* size 10: 111111110 */
    {0x1FF, 9 }, /* size 11: 111111111 — extended */
};

/* Chrominance DC size table (Table B.13) */
static const struct { uint16_t code; uint8_t len; } dc_chr[12] = {
    { 0x00, 2 }, /* size 0: 00 */
    { 0x01, 2 }, /* size 1: 01 */
    { 0x02, 2 }, /* size 2: 10 */
    { 0x06, 3 }, /* size 3: 110 */
    { 0x0E, 4 }, /* size 4: 1110 */
    { 0x1E, 5 }, /* size 5: 11110 */
    { 0x3E, 6 }, /* size 6: 111110 */
    { 0x7E, 7 }, /* size 7: 1111110 */
    { 0xFE, 8 }, /* size 8: 11111110 */
    {0x1FE, 9 }, /* size 9: 111111110 */
    {0x3FE,10 }, /* size 10: 1111111110 */
    {0x3FF,10 }, /* size 11: 1111111111 — extended */
};

/*
 * Encode a DC differential value as MPEG-2 intra DC VLC.
 * dct_dc_size (Huffman) + dct_dc_differential (size bits)
 */
static void encode_dc(BitWriter *bw, int dc_diff, int is_chroma) {
    int abs_val = dc_diff < 0 ? -dc_diff : dc_diff;
    int size = 0;
    { int tmp = abs_val; while (tmp > 0) { size++; tmp >>= 1; } }
    if (size > 11) size = 11;

    /* dct_dc_size VLC */
    if (is_chroma) {
        bw_put(bw, (uint32_t)dc_chr[size].code << (32 - dc_chr[size].len),
               dc_chr[size].len);
    } else {
        bw_put(bw, (uint32_t)dc_lum[size].code << (32 - dc_lum[size].len),
               dc_lum[size].len);
    }

    /* dct_dc_differential (size bits, ones' complement for negative) */
    if (size > 0) {
        int diff_code = (dc_diff >= 0) ? dc_diff
                                       : ((1 << size) - 1 + dc_diff);
        bw_put(bw, (uint32_t)diff_code << (32 - size), size);
    }
}

/*
 * Encode AC coefficient as 24-bit MPEG-2 escape code.
 * Format: 000001 (6b) | run (6b) | signed_level (12b)
 */
static inline void encode_ac_escape(BitWriter *bw, int run, int level) {
    /* Clamp level to 12-bit signed range */
    if (level > 2047) level = 2047;
    if (level < -2048) level = -2048;
    uint32_t escape = (0x01u << 18) | ((run & 0x3F) << 12) | (level & 0xFFF);
    bw_put(bw, escape << 8, 24);
}

/* End of Block: '10' (2 bits) */
#define VLC_EOB_CODE   0x2u
#define VLC_EOB_BITS   2

/* ── Aligned DMA buffers ─────────────────────────────────────────── */

/* Max VLC bitstream: ~24 bits/coeff × 64 coeffs × 6 blocks ≈ 1152 bytes
 * Plus headers/EOBs/padding. 4096 bytes is generous. */
static uint32_t ipu_vlc_buf[1024] __attribute__((aligned(64)));

/* IPU RGB32 output: 16×16×4 = 1024 bytes = 64 QW */
static uint32_t ipu_rgb_buf[256]  __attribute__((aligned(64)));

/* Raw MDEC quant tables (stored in zigzag order, as received from PSX) */
static uint8_t  ipu_qt_y[64]  __attribute__((aligned(16)));
static uint8_t  ipu_qt_uv[64] __attribute__((aligned(16)));
static int      ipu_qt_loaded = 0;

/* Pre-computed identity quant table (W[k]=8) for IPU SETIQ */
static uint8_t  ipu_identity_qt[64] __attribute__((aligned(16)));

/* ── IPU hardware interface ──────────────────────────────────────── */

static void ipu_reset(void) {
    /* Reset IPU */
    *R_EE_IPU_CTRL = IPU_CTRL_RST;
    while (*R_EE_IPU_CTRL & IPU_CTRL_BUSY)
        ;

    /* Set IDP=2 (10-bit DC precision), all other bits zero (QST=0, MP1=0, IVF=0, AS=0) */
    *R_EE_IPU_CTRL = IPU_CTRL_IDP_10BIT;

    /* BCLR: clear input FIFO */
    *R_EE_IPU_CMD = IPU_CMD_BCLR;
    while (*R_EE_IPU_CTRL & IPU_CTRL_BUSY)
        ;
}

/* Load quantization table to IPU via SETIQ */
static void ipu_load_qt(const uint8_t *qt, int iqm) {
    while (*R_EE_IPU_CTRL & IPU_CTRL_BUSY)
        ;
    *R_EE_IPU_CMD = IPU_CMD_BCLR;
    while (*R_EE_IPU_CTRL & IPU_CTRL_BUSY)
        ;

    /* Write 64 bytes (4 quadwords) to IPU input FIFO via sq instruction */
    const uint32_t *q = (const uint32_t *)qt;
    for (int i = 0; i < 4; i++) {
        __asm__ volatile(
            "lq $2, 0(%0)\n"
            "sq $2, 0(%1)\n"
            :
            : "r"(&q[i * 4]), "r"(A_EE_IPU_in_FIFO)
            : "$2", "memory"
        );
    }

    *R_EE_IPU_CMD = IPU_CMD_SETIQ | ((iqm & 1) << 27);
    while (*R_EE_IPU_CTRL & IPU_CTRL_BUSY)
        ;
}

/* Wait for DMA channel 3 (fromIPU) */
static inline void wait_d3(void) {
    while (*R_EE_D3_CHCR & 0x100) ;
}

/* Wait for DMA channel 4 (toIPU) */
static inline void wait_d4(void) {
    while (*R_EE_D4_CHCR & 0x100) ;
}

/* ── Core: parse one macroblock's RLE, dequant, encode VLC, IDEC ── */

/*
 * MDEC dequantization formulas (from psx-spx / our mdec.c):
 *   DC: val = DC_raw * qt[0]                (no q_scale, no /8)
 *   AC: val = (level * qt[k] * q_scale + 4) / 8
 *   Both clipped to [-1024, +1023]
 *
 * With identity IPU quant (W[k]=8, quantizer_scale=2):
 *   IPU AC dequant: (escape_level * 2 * 8) / 16 = escape_level
 *   IPU DC: F[0] = (pred + dc_diff) << (3 - IDP) = (512 + dc_diff) * 2
 *
 * So: escape_level = MDEC_dequant_val (AC),  dc_diff = MDEC_dequant_DC/2 - 512
 */

static inline int clip_1024(int v) {
    if (v < -1024) return -1024;
    if (v >  1023) return  1023;
    return v;
}

/*
 * Decode one macroblock via IPU IDEC.
 *
 * Parses 6 MDEC blocks (Cr, Cb, Y1-Y4), dequants in software,
 * re-encodes as MPEG-2 intra VLC, feeds to IPU for IDCT + CSC.
 *
 * Returns: pointer past consumed RLE data.
 */
static const uint16_t *ipu_decode_macroblock(const uint16_t *rl,
                                              const uint8_t *qt_y,
                                              const uint8_t *qt_uv,
                                              int signed_out)
{
    /* ── Step 1: Parse all 6 blocks in MDEC order, dequant ──────── */
    struct {
        int dc_dequant;        /* Dequantized DC value */
        int is_chroma;
        struct { int run; int level_dequant; } ac[63];
        int n_ac;
    } blocks[6];

    const uint16_t *p = rl;

    for (int i = 0; i < 6; i++) {
        const uint8_t *qt = (i < 2) ? qt_uv : qt_y;
        blocks[i].is_chroma = (i < 2) ? 1 : 0;

        /* Skip FE00 padding */
        while (*p == MDEC_END_OF_DATA) p++;

        uint16_t hw = *p++;
        int q_scale = RLE_RUN(hw);
        int dc_raw  = RLE_VAL(hw);

        /* DC dequant: val = DC_raw * qt[0], clip to [-1024,+1023] */
        blocks[i].dc_dequant = clip_1024(dc_raw * qt[0]);
        blocks[i].n_ac = 0;

        /* AC dequant: val = (level * qt[k] * q_scale + 4) / 8 */
        int k = 0;
        while (k < 63) {
            hw = *p++;
            if (hw == MDEC_END_OF_DATA) break;
            int run   = RLE_RUN(hw);
            int level = RLE_VAL(hw);
            k += run + 1;
            if (k >= 64) break;

            int dq = clip_1024((level * qt[k] * q_scale + 4) / 8);

            if (dq != 0 && blocks[i].n_ac < 63) {
                blocks[i].ac[blocks[i].n_ac].run = run;
                blocks[i].ac[blocks[i].n_ac].level_dequant = dq;
                blocks[i].n_ac++;
            }
        }
    }

    /* ── Step 2: Encode as MPEG-2 VLC bitstream ─────────────────── */

    BitWriter bw;
    bw_init(&bw, ipu_vlc_buf);

    /*
     * MPEG-2 I-type macroblock header:
     *   macroblock_type = '1' (1 bit) → MACROBLOCK_INTRA, no quant override
     *   No dct_type bit because DTD=0 in IDEC → frame_pred_frame_dct=1
     */
    bw_put(&bw, 0x80000000u, 1);  /* '1' = MACROBLOCK_INTRA */

    /* Block order: MPEG = Y1(2), Y2(3), Y3(4), Y4(5), Cb(1), Cr(0) */
    static const int mpeg_order[6] = { 2, 3, 4, 5, 1, 0 };
    /* DC predictor component index: 0=Y, 1=Cb, 2=Cr */
    static const int dc_cc[6] = { 0, 0, 0, 0, 1, 2 };

    /* DC predictors: init = 128 << IDP = 128 << 2 = 512 */
    int dc_pred[3] = { 512, 512, 512 };

    for (int idx = 0; idx < 6; idx++) {
        int bi = mpeg_order[idx];
        int cc = dc_cc[idx];
        int is_chroma = blocks[bi].is_chroma;

        /*
         * DC: F[0] = (pred + diff) << (3 - IDP) = (pred + diff) * 2
         * We want F[0] = blocks[bi].dc_dequant (+ 1024 for unsigned)
         *
         * The IPU's IDEC does NOT add +128 bias to IDCT output.
         * MDEC adds +128 per pixel for unsigned mode.
         * DC contribution per pixel = DCTblock[0] / 8.
         * To add 128 per pixel: DCTblock[0] += 128 * 8 = 1024.
         */
        int dc_target = blocks[bi].dc_dequant;
        if (!signed_out)
            dc_target += 1024;  /* shift to unsigned range for IPU */
        int dc_half = (dc_target >= 0) ? (dc_target / 2) : -(((-dc_target) + 1) / 2);
        int dc_diff = dc_half - dc_pred[cc];
        dc_pred[cc] = dc_half;  /* Update predictor */

        /* Clamp dc_diff to encodable range (size ≤ 11 → ±2047) */
        if (dc_diff > 2047) dc_diff = 2047;
        if (dc_diff < -2047) dc_diff = -2047;

        encode_dc(&bw, dc_diff, is_chroma);

        /* AC coefficients — all as 24-bit escape codes */
        for (int j = 0; j < blocks[bi].n_ac; j++) {
            encode_ac_escape(&bw, blocks[bi].ac[j].run,
                              blocks[bi].ac[j].level_dequant);
        }

        /* EOB */
        bw_put(&bw, VLC_EOB_CODE << 30, VLC_EOB_BITS);
    }

    /*
     * Slice termination: after the macroblock, IPU reads MBA (macroblock
     * address increment).  If the next 16 bits are < 0x20, UBITS(11) < 1,
     * which falls to 'default' → finish_idec.  Then it peeks 8 bits:
     * if bit8 != 0, skips to reading 32-bit TOP.
     *
     * We emit: 20+ zero bits (MBA fail) + 0xFF marker (bit8 != 0) + 32-bit pad
     */
    /* Flush current word */
    bw_flush(&bw);
    /* Zero padding words (for MBA fail: need ≥16 zero bits) */
    ipu_vlc_buf[bw.words++] = 0;  /* 32 zero bits */
    /* 0xFF marker byte at MSB (big-endian: 0xFF000000 after bswap → byte 0xFF) */
    ipu_vlc_buf[bw.words++] = __builtin_bswap32(0xFF000000u);
    /* 32-bit pad for ipuRegs.top read */
    ipu_vlc_buf[bw.words++] = 0;

    /* Pad to quadword boundary (16 bytes) */
    while (bw.words & 3)
        ipu_vlc_buf[bw.words++] = 0;

    int vlc_qwc = bw.words / 4;

    /* ── Step 3: Feed VLC to IPU via DMA ch4, output via DMA ch3 ── */
    FlushCache(0);

    /* BCLR: clear input FIFO */
    *R_EE_IPU_CMD = IPU_CMD_BCLR;
    while (*R_EE_IPU_CTRL & IPU_CTRL_BUSY);

    /* Issue IDEC command: QSC=2, SGN=signed_out, OFM=0 (RGB32), DTD=0 */
    uint32_t idec_cmd = IPU_CMD_IDEC
                      | ((signed_out & 1) << 25)
                      | (2u << 16);
    *R_EE_IPU_CMD = idec_cmd;

    /* Start DMA ch3 (fromIPU): receive RGB32 output (64 QW) */
    *R_EE_D3_MADR = (uint32_t)(uintptr_t)ipu_rgb_buf;
    *R_EE_D3_QWC  = 64;
    *R_EE_D3_CHCR = 0x100;  /* DIR=to_MEM, normal, STR */

    /* Start DMA ch4 (toIPU): feed VLC data from memory to IPU FIFO */
    *R_EE_D4_MADR = (uint32_t)(uintptr_t)ipu_vlc_buf;
    *R_EE_D4_QWC  = vlc_qwc;
    *R_EE_D4_CHCR = 0x101;  /* DIR=from_MEM, normal, STR */

    /* Wait for DMA ch3 (output received = IDEC decoded the macroblock) */
    wait_d3();

    /* Stop DMA ch4 if still running (termination data may keep it alive) */
    *R_EE_D4_CHCR = 0;

    /* Force-reset IPU to abort the IDEC command cleanly.
     * After D3 completes, IDEC has decoded the macroblock but is still
     * parsing MBA/finish — it stalls waiting for FIFO data.  RST aborts it. */
    *R_EE_IPU_CTRL = (1u << 30);  /* RST bit */
    while (*R_EE_IPU_CTRL & (1u << 30));  /* wait RST to self-clear */

    /* Restore IPU state lost by RST: IDP=2, identity quant table */
    *R_EE_IPU_CTRL = IPU_CTRL_IDP_10BIT;
    ipu_load_qt(ipu_identity_qt, 0);

    FlushCache(0);

    return p;
}

/* ── Format converters ───────────────────────────────────────────── */

/*
 * IPU RGB32 pixel layout: R(7:0), G(15:8), B(23:16), A(31:24)
 * PSX 15-bit: R(4:0), G(9:5), B(14:10), STP(15)
 */
static void rgb32_to_rgb15(const uint32_t *rgb32, uint16_t *rgb15,
                           int stp_bit) {
    uint16_t stp = stp_bit ? 0x8000 : 0;
    for (int i = 0; i < 256; i++) {
        uint32_t px = rgb32[i];
        uint8_t r = (px >>  0) & 0xFF;
        uint8_t g = (px >>  8) & 0xFF;
        uint8_t b = (px >> 16) & 0xFF;
        rgb15[i] = ((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)) | stp;
    }
}

static void rgb32_to_rgb24(const uint32_t *rgb32, uint8_t *rgb24) {
    for (int i = 0; i < 256; i++) {
        uint32_t px = rgb32[i];
        rgb24[i * 3 + 0] = (px >>  0) & 0xFF;  /* R */
        rgb24[i * 3 + 1] = (px >>  8) & 0xFF;  /* G */
        rgb24[i * 3 + 2] = (px >> 16) & 0xFF;  /* B */
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

void MDEC_IPU_Init(void) {
    memset(ipu_identity_qt, 8, 64);
    ipu_reset();
    ipu_qt_loaded = 0;
    uint32_t ctrl = *R_EE_IPU_CTRL;
    printf("[MDEC-IPU] Init: CTRL=0x%08X IDP=%d\n", ctrl, (ctrl >> 16) & 3);
}

void MDEC_IPU_LoadQuantTable(const uint8_t *qt_y_in, const uint8_t *qt_uv_in) {
    memcpy(ipu_qt_y, qt_y_in, 64);
    memcpy(ipu_qt_uv, qt_uv_in, 64);

    /*
     * Identity quant table: W[k] = 8 for all k.
     * With quantizer_scale = 2 (IDEC QSC=2):
     *   F[k] = (level * 2 * 8) / 16 = level
     * This makes the IPU's dequant a no-op — we pre-dequant in software.
     */
    uint8_t identity_qt[64];
    memset(identity_qt, 8, 64);

    ipu_load_qt(identity_qt, 0);  /* intra matrix */
    ipu_load_qt(identity_qt, 1);  /* non-intra (shouldn't be used, load anyway) */
    ipu_qt_loaded = 1;

    printf("[MDEC-IPU] Quant loaded (identity W[k]=8), qt_y[0]=%d qt_uv[0]=%d\n",
           qt_y_in[0], qt_uv_in[0]);
}

int MDEC_IPU_DecodeDMA1(const uint16_t **rl, const uint16_t *rl_end,
                         uint8_t *image, uint32_t size,
                         int depth, int signed_out, int stp_bit)
{
    if (depth < 2) return -1;  /* only 15-bit and 24-bit modes */
    if (!ipu_qt_loaded) {
        printf("[MDEC-IPU] quant not loaded, fallback\n");
        return -1;
    }

    printf("[MDEC-IPU] DecodeDMA1 enter: depth=%d size=%u\n", depth, (unsigned)size);

    uint32_t block_bytes = (depth == 2) ? (16*16*3) : (16*16*2);
    uint32_t written = 0;
    int mb_count = 0;

    while (size >= block_bytes && *rl < rl_end) {
        *rl = ipu_decode_macroblock(*rl, ipu_qt_y, ipu_qt_uv, signed_out);

        if (depth == 2)
            rgb32_to_rgb24(ipu_rgb_buf, image);
        else
            rgb32_to_rgb15(ipu_rgb_buf, (uint16_t *)image, stp_bit);

        image   += block_bytes;
        size    -= block_bytes;
        written += block_bytes;
        mb_count++;
    }

    printf("[MDEC-IPU] Decoded %d macroblocks, %u bytes written\n",
           mb_count, (unsigned)written);
    return (int)written;
}

#endif /* ENABLE_MDEC_IPU */
