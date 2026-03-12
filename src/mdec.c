/*
 * MDEC (Motion Decoder) — Software PSX MDEC emulation
 *
 * Phase 1: Full software decode (RLE → dequant → IDCT → YUV→RGB).
 * Phase 2 will replace IDCT+YUV→RGB with PS2 IPU hardware (Approach B).
 *
 * Reference: PCSX-ReARMed libpcsxcore/mdec.c, DuckStation mdec.cpp
 */

#include "mdec.h"
#include "superpsx.h"
#include "scheduler.h"
#include "dynarec.h" /* jit_invalidate_page */
#include <string.h>
#include <stdio.h>

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "MDEC"

/* ---------- Constants / Macros ---------- */

#define DSIZE   8
#define DSIZE2  (DSIZE * DSIZE)   /* 64 */

/* MDEC timing: cycles per output byte (PCSX-ReARMed uses MDEC_BIAS=14) */
#define MDEC_BIAS  14
#define MDEC_DELAY 1024

/* RLE helpers */
#define RLE_RUN(hw)  ((hw) >> 10)
#define RLE_VAL(hw)  (((int)(hw) << (sizeof(int)*8 - 10)) >> (sizeof(int)*8 - 10))

/* Fixed-point IDCT */
#define AAN_CONST_BITS       12
#define AAN_PRESCALE_BITS    16
#define AAN_CONST_SIZE       24
#define AAN_CONST_SCALE      (AAN_CONST_SIZE - AAN_CONST_BITS)
#define AAN_PRESCALE_SIZE    20
#define AAN_PRESCALE_SCALE   (AAN_PRESCALE_SIZE - AAN_PRESCALE_BITS)
#define AAN_EXTRA            12

#define SCALE(x, n)    ((x) >> (n))
#define SCALER(x, n)   (((x) + ((1 << (n)) >> 1)) >> (n))
#define MULS(var, c)   (SCALE((var) * (c), AAN_CONST_BITS))

#define FIX_1_082392200  SCALER(18159528, AAN_CONST_SCALE)
#define FIX_1_414213562  SCALER(23726566, AAN_CONST_SCALE)
#define FIX_1_847759065  SCALER(31000253, AAN_CONST_SCALE)
#define FIX_2_613125930  SCALER(43840978, AAN_CONST_SCALE)

#define MDEC_END_OF_DATA 0xFE00

/* Command register flags */
#define MDEC0_STP     0x02000000
#define MDEC0_RGB24   0x08000000

/* Status register flags */
#define MDEC1_BUSY    0x20000000
#define MDEC1_DREQ    0x18000000
#define MDEC1_FIFO    0xC0000000
#define MDEC1_RGB24   0x02000000
#define MDEC1_STP     0x00800000
#define MDEC1_RESET   0x80000000

/* Output sizes */
#define SIZE_OF_4B_BLOCK   (8 * 8 / 2)     /* 32 bytes (mono 4-bit) */
#define SIZE_OF_8B_BLOCK   (8 * 8)         /* 64 bytes (mono 8-bit) */
#define SIZE_OF_16B_BLOCK  (16 * 16 * 2)  /* 512 bytes */
#define SIZE_OF_24B_BLOCK  (16 * 16 * 3)  /* 768 bytes */

/* ---------- Zigzag + IDCT scale tables ---------- */

static const int zscan[DSIZE2] = {
    0,  1,  8, 16,  9,  2,  3, 10,
   17, 24, 32, 25, 18, 11,  4,  5,
   12, 19, 26, 33, 40, 48, 41, 34,
   27, 20, 13,  6,  7, 14, 21, 28,
   35, 42, 49, 56, 57, 50, 43, 36,
   29, 22, 15, 23, 30, 37, 44, 51,
   58, 59, 52, 45, 38, 31, 39, 46,
   53, 60, 61, 54, 47, 55, 62, 63
};

static const int aanscales[DSIZE2] = {
    1048576, 1454417, 1370031, 1232995, 1048576,  823861, 567485, 289301,
    1454417, 2017334, 1900287, 1710213, 1454417, 1142728, 787125, 401273,
    1370031, 1900287, 1790031, 1610986, 1370031, 1076426, 741455, 377991,
    1232995, 1710213, 1610986, 1449849, 1232995,  968758, 667292, 340183,
    1048576, 1454417, 1370031, 1232995, 1048576,  823861, 567485, 289301,
     823861, 1142728, 1076426,  968758,  823861,  647303, 445870, 227303,
     567485,  787125,  741455,  667292,  567485,  445870, 307121, 156569,
     289301,  401273,  377991,  340183,  289301,  227303, 156569,  79818
};

/* ---------- MDEC State ---------- */

/* State machine for register-write FIFO uploads */
enum {
    MDEC_STATE_IDLE = 0,
    MDEC_STATE_RECV_QUANT,   /* Receiving quant table words via 0x1F801820 */
    MDEC_STATE_RECV_SCALE,   /* Receiving IDCT scale table words */
    MDEC_STATE_RECV_DATA,    /* Receiving compressed data words (manual decode) */
};

static struct {
    uint32_t reg0;                /* Command register */
    uint32_t reg1;                /* Status register */
    const uint16_t *rl;           /* Current RLE read pointer */
    const uint16_t *rl_end;       /* End of RLE data */
    uint8_t *block_buffer_pos;    /* Partial block output position (DMA1) */
    uint8_t  block_buffer[SIZE_OF_24B_BLOCK]; /* Partial output cache */
    struct {
        uint32_t adr, bcr, chcr;
    } pending_dma1;               /* Deferred DMA1 request */

    /* Output FIFO for manual (non-DMA) reads */
    int out_pos;                  /* Read position in block_buffer (bytes) */
    int out_end;                  /* Total bytes in current decoded block */

    /* Remaining parameter words for the current command */
    int remaining_words;

    /* Register-write FIFO state */
    int fifo_state;               /* MDEC_STATE_xxx */
    int fifo_remaining;           /* Words still expected */
    uint8_t qt_buffer[128];       /* Quant table staging (Y 64B + UV 64B) */
    int qt_pos;                   /* Bytes written so far */
    int qt_has_uv;                /* Command specified UV table too */

    /* Scale table staging for register-write path */
    uint16_t scale_staging[64];   /* Accumulate 64 halfwords */
    int scale_pos;                /* Halfwords written so far */
} mdec;

static int iq_y[DSIZE2];         /* Combined quant+scale for luminance */
static int iq_uv[DSIZE2];        /* Combined quant+scale for chrominance */
static int aan_only[DSIZE2];     /* AAN prescale without quant (for q_scale=0) */
static int16_t scale_table[DSIZE2]; /* IDCT scale table (stored transposed) */

/* Input buffer for manual (non-DMA) word-by-word writes */
#define MDEC_IN_BUF_SIZE  8192  /* halfwords */
static uint16_t mdec_in_buf[MDEC_IN_BUF_SIZE];
static int mdec_in_write_idx;

/* Forward declarations */
static void mdec_decode_out_block(void);

/* ---------- Quant table setup ---------- */

static void iqtab_init(int *iqtab, const uint8_t *qt) {
    for (int i = 0; i < DSIZE2; i++)
        iqtab[i] = qt[i] * SCALER(aanscales[zscan[i]], AAN_PRESCALE_SCALE);
}

/* AAN prescale without quant (for q_scale=0 mode, linear order) */
static void aan_only_init(void) {
    for (int i = 0; i < DSIZE2; i++)
        aan_only[i] = SCALER(aanscales[i], AAN_PRESCALE_SCALE);
}

/* ---------- IDCT ---------- */

/* Reference IDCT using the scale table (DuckStation-style two-pass matrix multiply).
 * Used for q_scale=0 blocks where AAN prescaling is not applied.
 * Scale table must be stored WITHOUT transposition (direct from MDEC(3)). */
static void real_idct_core(int *blk) {
    int tmp[DSIZE2];
    /* Pass 1: column transform */
    for (int x = 0; x < DSIZE; x++) {
        for (int y = 0; y < DSIZE; y++) {
            int sum = 0;
            for (int z = 0; z < DSIZE; z++)
                sum += blk[x + z * DSIZE] * (scale_table[y + z * DSIZE] >> 3);
            tmp[x + y * DSIZE] = (sum + 0xFFF) >> 13;
        }
    }
    /* Pass 2: row transform */
    for (int x = 0; x < DSIZE; x++) {
        for (int y = 0; y < DSIZE; y++) {
            int sum = 0;
            for (int z = 0; z < DSIZE; z++)
                sum += tmp[z + y * DSIZE] * (scale_table[x + z * DSIZE] >> 3);
            blk[x + y * DSIZE] = (sum + 0xFFF) >> 13;
        }
    }
}

static void idct(int *blk, int used_col) {
    int tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
    int z5, z10, z11, z12, z13;
    int *ptr;

    /* All-DC shortcut */
    if (used_col == -1) {
        int v = blk[0];
        for (int i = 0; i < DSIZE2; i++)
            blk[i] = v;
        return;
    }

    /* Column pass */
    ptr = blk;
    for (int i = 0; i < DSIZE; i++, ptr++) {
        if ((used_col & (1 << i)) == 0) {
            if (ptr[DSIZE * 0]) {
                int v = ptr[0];
                for (int j = 0; j < DSIZE; j++)
                    ptr[DSIZE * j] = v;
                used_col |= (1 << i);
            }
            continue;
        }

        z10 = ptr[DSIZE * 0] + ptr[DSIZE * 4];
        z11 = ptr[DSIZE * 0] - ptr[DSIZE * 4];
        z13 = ptr[DSIZE * 2] + ptr[DSIZE * 6];
        z12 = MULS(ptr[DSIZE * 2] - ptr[DSIZE * 6], FIX_1_414213562) - z13;

        tmp0 = z10 + z13;
        tmp3 = z10 - z13;
        tmp1 = z11 + z12;
        tmp2 = z11 - z12;

        z13 = ptr[DSIZE * 3] + ptr[DSIZE * 5];
        z10 = ptr[DSIZE * 3] - ptr[DSIZE * 5];
        z11 = ptr[DSIZE * 1] + ptr[DSIZE * 7];
        z12 = ptr[DSIZE * 1] - ptr[DSIZE * 7];

        tmp7 = z11 + z13;
        z5 = (z12 - z10) * FIX_1_847759065;
        tmp6 = SCALE(z10 * FIX_2_613125930 + z5, AAN_CONST_BITS) - tmp7;
        tmp5 = MULS(z11 - z13, FIX_1_414213562) - tmp6;
        tmp4 = SCALE(z12 * FIX_1_082392200 - z5, AAN_CONST_BITS) + tmp5;

        ptr[DSIZE * 0] = tmp0 + tmp7;
        ptr[DSIZE * 7] = tmp0 - tmp7;
        ptr[DSIZE * 1] = tmp1 + tmp6;
        ptr[DSIZE * 6] = tmp1 - tmp6;
        ptr[DSIZE * 2] = tmp2 + tmp5;
        ptr[DSIZE * 5] = tmp2 - tmp5;
        ptr[DSIZE * 4] = tmp3 + tmp4;
        ptr[DSIZE * 3] = tmp3 - tmp4;
    }

    /* Row pass */
    ptr = blk;
    if (used_col == 1) {
        /* Only column 0 was non-zero — fill each row from its first element */
        for (int i = 0; i < DSIZE; i++, ptr += DSIZE) {
            int v = ptr[0];
            ptr[1] = ptr[2] = ptr[3] = ptr[4] = ptr[5] = ptr[6] = ptr[7] = v;
        }
    } else {
    for (int i = 0; i < DSIZE; i++, ptr += DSIZE) {
        z10 = ptr[0] + ptr[4];
        z11 = ptr[0] - ptr[4];
        z13 = ptr[2] + ptr[6];
        z12 = MULS(ptr[2] - ptr[6], FIX_1_414213562) - z13;

        tmp0 = z10 + z13;
        tmp3 = z10 - z13;
        tmp1 = z11 + z12;
        tmp2 = z11 - z12;

        z13 = ptr[3] + ptr[5];
        z10 = ptr[3] - ptr[5];
        z11 = ptr[1] + ptr[7];
        z12 = ptr[1] - ptr[7];

        tmp7 = z11 + z13;
        z5 = (z12 - z10) * FIX_1_847759065;
        tmp6 = SCALE(z10 * FIX_2_613125930 + z5, AAN_CONST_BITS) - tmp7;
        tmp5 = MULS(z11 - z13, FIX_1_414213562) - tmp6;
        tmp4 = SCALE(z12 * FIX_1_082392200 - z5, AAN_CONST_BITS) + tmp5;

        ptr[0] = tmp0 + tmp7;
        ptr[7] = tmp0 - tmp7;
        ptr[1] = tmp1 + tmp6;
        ptr[6] = tmp1 - tmp6;
        ptr[2] = tmp2 + tmp5;
        ptr[5] = tmp2 - tmp5;
        ptr[4] = tmp3 + tmp4;
        ptr[3] = tmp3 - tmp4;
    }
    } /* else (full row pass) */
}

/* ---------- RLE → blocks ---------- */

static const uint16_t *rl2blk(int *blk, const uint16_t *rl) {
    int i, k, q_scale, used_col;
    int *iqtab;

    memset(blk, 0, 6 * DSIZE2 * sizeof(int));
    iqtab = iq_uv;

    for (i = 0; i < 6; i++) {
        /* Blocks: Cr, Cb, Y1, Y2, Y3, Y4 */
        if (i == 2) iqtab = iq_y;

        /* Skip FE00 padding between blocks */
        while (*rl == MDEC_END_OF_DATA) rl++;

        uint16_t hw = *rl++;
        q_scale = RLE_RUN(hw);
        k = 0;
        used_col = 0;

        if (q_scale == 0) {
            /* Special mode: val*2, linear order, no quant table.
             * Use real_idct_core for accuracy. */
            int val = RLE_VAL(hw) * 2;
            if (val < -0x400) val = -0x400; else if (val > 0x3FF) val = 0x3FF;
            blk[0] = val;
            while (k < 63) {
                hw = *rl++;
                k += RLE_RUN(hw) + 1;
                if (k < DSIZE2) {
                    val = RLE_VAL(hw) * 2;
                    if (val < -0x400) val = -0x400; else if (val > 0x3FF) val = 0x3FF;
                    blk[k] = val;
                }
            }
            real_idct_core(blk);
            /* Scale up to match AAN output range (yuv functions expect >>10) */
            for (int j = 0; j < DSIZE2; j++)
                blk[j] <<= 10;
        } else {
            /* Normal mode: val*qt*q_scale, zigzag order */
            blk[0] = SCALER(RLE_VAL(hw) * iqtab[0], AAN_EXTRA - 3);
            while (k < 63) {
                hw = *rl++;
                k += RLE_RUN(hw) + 1;
                if (k < DSIZE2) {
                    int val = RLE_VAL(hw);
                    blk[zscan[k]] = SCALER(val * iqtab[k] * q_scale, AAN_EXTRA);
                    used_col |= (zscan[k] > 7) ? 1 << (zscan[k] & 7) : 0;
                }
            }
        }

        if (q_scale > 0) {
            if (k == 0) used_col = -1;
            /* IDCT this block (AAN fast path) */
            idct(blk, used_col);
        }
        blk += DSIZE2;
    }
    return rl;
}

/* ---------- Mono RLE → single Y block ---------- */

static const uint16_t *rl2blk_mono(int *blk, const uint16_t *rl) {
    int k, q_scale, used_col;

    memset(blk, 0, DSIZE2 * sizeof(int));

    /* Skip FE00 padding */
    while (*rl == MDEC_END_OF_DATA) rl++;

    uint16_t hw = *rl++;
    q_scale = RLE_RUN(hw);
    k = 0;
    used_col = 0;

    if (q_scale == 0) {
        /* Special mode: val*2, linear order, no quant table.
         * Use real_idct_core (scale-table IDCT) for accuracy. */
        int val = RLE_VAL(hw) * 2;
        if (val < -0x400) val = -0x400; else if (val > 0x3FF) val = 0x3FF;
        blk[0] = val;
        while (k < 63) {
            hw = *rl++;
            k += RLE_RUN(hw) + 1;
            if (k < DSIZE2) {
                val = RLE_VAL(hw) * 2;
                if (val < -0x400) val = -0x400; else if (val > 0x3FF) val = 0x3FF;
                blk[k] = val;
            }
        }
        real_idct_core(blk);
        /* Scale up to match AAN output range (y_to_mono expects >>10) */
        for (int i = 0; i < DSIZE2; i++)
            blk[i] <<= 10;
    } else {
        blk[0] = SCALER(RLE_VAL(hw) * iq_y[0], AAN_EXTRA - 3);
        while (k < 63) {
            hw = *rl++;
            k += RLE_RUN(hw) + 1;
            if (k < DSIZE2) {
                int val = RLE_VAL(hw);
                blk[zscan[k]] = SCALER(val * iq_y[k] * q_scale, AAN_EXTRA);
                used_col |= (zscan[k] > 7) ? 1 << (zscan[k] & 7) : 0;
            }
        }
    }
    if (q_scale > 0) {
        if (k == 0) used_col = -1;
        idct(blk, used_col);
    }
    return rl;
}

/* ---------- Y → Mono (4-bit / 8-bit) ---------- */

static void y_to_mono8(int *blk, uint8_t *image, int signed_out) {
    for (int i = 0; i < DSIZE2; i++) {
        int v = SCALER(blk[i], 10);
        if (!signed_out) v += 128;
        image[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

static void y_to_mono4(int *blk, uint8_t *image, int signed_out) {
    for (int i = 0; i < DSIZE2; i += 2) {
        int v0 = SCALER(blk[i], 10);
        int v1 = SCALER(blk[i + 1], 10);
        if (!signed_out) { v0 += 128; v1 += 128; }
        v0 = v0 < 0 ? 0 : (v0 > 255 ? 255 : v0);
        v1 = v1 < 0 ? 0 : (v1 > 255 ? 255 : v1);
        image[i / 2] = (uint8_t)(((v0 >> 4) & 0x0F) | (v1 & 0xF0));
    }
}

/* ---------- YUV→RGB (15-bit / 24-bit) ---------- */

/* Color coefficients (fixed-point, matching PCSX-ReARMed) */
#define MULR(a)    ((1434 * (a)))
#define MULB(a)    ((1807 * (a)))
#define MULG(a)    ((-351 * (a)))
#define MULG2(a,b) ((-351 * (a) - 728 * (b)))
#define MULY(a)    ((a) << 10)

#define SCALE8(c)  SCALER(c, 20)
#define SCALE5(c)  SCALER(c, 23)

static inline int clamp5(int v) {
    v += 16;
    return v < 0 ? 0 : (v > 31 ? 31 : v);
}

static inline int clamp8(int v) {
    v += 128;
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

#define CLAMP_SCALE5(a) clamp5(SCALE5(a))
#define CLAMP_SCALE8(a) clamp8(SCALE8(a))
#define MAKERGB15(r,g,b,a) ((uint16_t)((a)|((b)<<10)|((g)<<5)|(r)))

static void yuv2rgb15(int *blk, uint16_t *image) {
    int *Yblk  = blk + DSIZE2 * 2;
    int *Crblk = blk;
    int *Cbblk = blk + DSIZE2;
    uint16_t stp = (mdec.reg0 & MDEC0_STP) ? 0x8000 : 0;

    for (int y = 0; y < 16; y += 2, Crblk += 4, Cbblk += 4, Yblk += 8, image += 24) {
        if (y == 8) Yblk += DSIZE2;
        for (int x = 0; x < 4; x++, image += 2, Crblk++, Cbblk++, Yblk += 2) {
            int R, G, B, Y;

            /* Left half (Y1/Y3 block) */
            R = MULR(*Crblk);
            G = MULG2(*Cbblk, *Crblk);
            B = MULB(*Cbblk);

            Y = MULY(Yblk[0]);
            image[0] = MAKERGB15(CLAMP_SCALE5(Y+R), CLAMP_SCALE5(Y+G), CLAMP_SCALE5(Y+B), stp);
            Y = MULY(Yblk[1]);
            image[1] = MAKERGB15(CLAMP_SCALE5(Y+R), CLAMP_SCALE5(Y+G), CLAMP_SCALE5(Y+B), stp);
            Y = MULY(Yblk[8]);
            image[16] = MAKERGB15(CLAMP_SCALE5(Y+R), CLAMP_SCALE5(Y+G), CLAMP_SCALE5(Y+B), stp);
            Y = MULY(Yblk[9]);
            image[17] = MAKERGB15(CLAMP_SCALE5(Y+R), CLAMP_SCALE5(Y+G), CLAMP_SCALE5(Y+B), stp);

            /* Right half (Y2/Y4 block) */
            R = MULR(*(Crblk + 4));
            G = MULG2(*(Cbblk + 4), *(Crblk + 4));
            B = MULB(*(Cbblk + 4));

            Y = MULY(Yblk[DSIZE2]);
            image[8] = MAKERGB15(CLAMP_SCALE5(Y+R), CLAMP_SCALE5(Y+G), CLAMP_SCALE5(Y+B), stp);
            Y = MULY(Yblk[DSIZE2 + 1]);
            image[9] = MAKERGB15(CLAMP_SCALE5(Y+R), CLAMP_SCALE5(Y+G), CLAMP_SCALE5(Y+B), stp);
            Y = MULY(Yblk[DSIZE2 + 8]);
            image[24] = MAKERGB15(CLAMP_SCALE5(Y+R), CLAMP_SCALE5(Y+G), CLAMP_SCALE5(Y+B), stp);
            Y = MULY(Yblk[DSIZE2 + 9]);
            image[25] = MAKERGB15(CLAMP_SCALE5(Y+R), CLAMP_SCALE5(Y+G), CLAMP_SCALE5(Y+B), stp);
        }
    }
}

static void yuv2rgb24(int *blk, uint8_t *image) {
    int *Yblk  = blk + DSIZE2 * 2;
    int *Crblk = blk;
    int *Cbblk = blk + DSIZE2;

    for (int y = 0; y < 16; y += 2, Crblk += 4, Cbblk += 4, Yblk += 8, image += 8 * 3 * 3) {
        if (y == 8) Yblk += DSIZE2;
        for (int x = 0; x < 4; x++, image += 6, Crblk++, Cbblk++, Yblk += 2) {
            int R, G, B, Y;

            /* Left half */
            R = MULR(*Crblk);
            G = MULG2(*Cbblk, *Crblk);
            B = MULB(*Cbblk);

            Y = MULY(Yblk[0]);
            image[0] = CLAMP_SCALE8(Y + R);
            image[1] = CLAMP_SCALE8(Y + G);
            image[2] = CLAMP_SCALE8(Y + B);
            Y = MULY(Yblk[1]);
            image[3] = CLAMP_SCALE8(Y + R);
            image[4] = CLAMP_SCALE8(Y + G);
            image[5] = CLAMP_SCALE8(Y + B);
            Y = MULY(Yblk[8]);
            image[16 * 3 + 0] = CLAMP_SCALE8(Y + R);
            image[16 * 3 + 1] = CLAMP_SCALE8(Y + G);
            image[16 * 3 + 2] = CLAMP_SCALE8(Y + B);
            Y = MULY(Yblk[9]);
            image[17 * 3 + 0] = CLAMP_SCALE8(Y + R);
            image[17 * 3 + 1] = CLAMP_SCALE8(Y + G);
            image[17 * 3 + 2] = CLAMP_SCALE8(Y + B);

            /* Right half */
            R = MULR(*(Crblk + 4));
            G = MULG2(*(Cbblk + 4), *(Crblk + 4));
            B = MULB(*(Cbblk + 4));

            Y = MULY(Yblk[DSIZE2]);
            image[8 * 3 + 0] = CLAMP_SCALE8(Y + R);
            image[8 * 3 + 1] = CLAMP_SCALE8(Y + G);
            image[8 * 3 + 2] = CLAMP_SCALE8(Y + B);
            Y = MULY(Yblk[DSIZE2 + 1]);
            image[9 * 3 + 0] = CLAMP_SCALE8(Y + R);
            image[9 * 3 + 1] = CLAMP_SCALE8(Y + G);
            image[9 * 3 + 2] = CLAMP_SCALE8(Y + B);
            Y = MULY(Yblk[DSIZE2 + 8]);
            image[24 * 3 + 0] = CLAMP_SCALE8(Y + R);
            image[24 * 3 + 1] = CLAMP_SCALE8(Y + G);
            image[24 * 3 + 2] = CLAMP_SCALE8(Y + B);
            Y = MULY(Yblk[DSIZE2 + 9]);
            image[25 * 3 + 0] = CLAMP_SCALE8(Y + R);
            image[25 * 3 + 1] = CLAMP_SCALE8(Y + G);
            image[25 * 3 + 2] = CLAMP_SCALE8(Y + B);
        }
    }
}

/* ---------- Public API ---------- */

void MDEC_Init(void) {
    memset(&mdec, 0, sizeof(mdec));
    memset(iq_y, 0, sizeof(iq_y));
    memset(iq_uv, 0, sizeof(iq_uv));
    aan_only_init();
    mdec.fifo_state = MDEC_STATE_IDLE;
}

void MDEC_WriteCommand(uint32_t data) {
    /* If we're in a FIFO-receiving state, this write is parameter data */
    if (mdec.fifo_state == MDEC_STATE_RECV_DATA) {
        /* Append 2 halfwords to the manual input buffer */
        if (mdec_in_write_idx + 2 <= MDEC_IN_BUF_SIZE) {
            mdec_in_buf[mdec_in_write_idx++] = (uint16_t)(data & 0xFFFF);
            mdec_in_buf[mdec_in_write_idx++] = (uint16_t)(data >> 16);
            mdec.rl_end = &mdec_in_buf[mdec_in_write_idx];
        }
        if (mdec.remaining_words > 0) {
            mdec.remaining_words--;
            if (mdec.remaining_words == 0)
                mdec.fifo_state = MDEC_STATE_IDLE;
        }
        return;
    }
    if (mdec.fifo_state == MDEC_STATE_RECV_QUANT) {
        /* Pack 32-bit word into qt_buffer as 4 bytes (little-endian) */
        if (mdec.qt_pos + 4 <= (int)sizeof(mdec.qt_buffer)) {
            mdec.qt_buffer[mdec.qt_pos + 0] = (uint8_t)(data);
            mdec.qt_buffer[mdec.qt_pos + 1] = (uint8_t)(data >> 8);
            mdec.qt_buffer[mdec.qt_pos + 2] = (uint8_t)(data >> 16);
            mdec.qt_buffer[mdec.qt_pos + 3] = (uint8_t)(data >> 24);
            mdec.qt_pos += 4;
        }
        mdec.fifo_remaining--;
        if (mdec.fifo_remaining <= 0) {
            /* Apply quant table(s) */
            iqtab_init(iq_y, mdec.qt_buffer);
            if (mdec.qt_has_uv)
                iqtab_init(iq_uv, mdec.qt_buffer + 64);
            else
                iqtab_init(iq_uv, mdec.qt_buffer); /* same table for UV */
            mdec.fifo_state = MDEC_STATE_IDLE;
        }
        return;
    }
    if (mdec.fifo_state == MDEC_STATE_RECV_SCALE) {
        /* Accumulate scale table data */
        if (mdec.scale_pos + 2 <= 64) {
            mdec.scale_staging[mdec.scale_pos++] = (uint16_t)(data & 0xFFFF);
            mdec.scale_staging[mdec.scale_pos++] = (uint16_t)(data >> 16);
        }
        mdec.fifo_remaining--;
        if (mdec.fifo_remaining <= 0) {
            /* Store scale table directly (no transposition — matches DuckStation) */
            for (int i = 0; i < DSIZE2; i++)
                scale_table[i] = (int16_t)mdec.scale_staging[i];
            mdec.fifo_state = MDEC_STATE_IDLE;
        }
        return;
    }

    /* New command word */
    mdec.reg0 = data;

    uint32_t cmd = (data >> 29) & 7;

    switch (cmd) {
    case 1: /* Decode Macroblocks */
        mdec.reg1 |= MDEC1_BUSY;
        mdec.remaining_words = data & 0xFFFF;
        if (mdec.remaining_words == 0) mdec.remaining_words = 0x10000;
        mdec.out_pos = 0;
        mdec.out_end = 0;
        /* Set up manual input buffer for word-by-word writes */
        mdec_in_write_idx = 0;
        mdec.rl = mdec_in_buf;
        mdec.rl_end = mdec_in_buf;
        mdec.fifo_state = MDEC_STATE_RECV_DATA;
        break;
    case 2: /* Set Quant Table — bit0: 0=Y only (16 words), 1=Y+UV (32 words) */
        mdec.fifo_state = MDEC_STATE_RECV_QUANT;
        mdec.qt_has_uv = (data & 1) ? 1 : 0;
        mdec.fifo_remaining = mdec.qt_has_uv ? 32 : 16;
        mdec.qt_pos = 0;
        break;
    case 3: /* Set Scale Table — always 32 words (64 halfwords) */
        mdec.fifo_state = MDEC_STATE_RECV_SCALE;
        mdec.fifo_remaining = 32;
        mdec.scale_pos = 0;
        break;
    default:
        /* Decode or other — command is stored in reg0, DMA0 handles actual data */
        break;
    }
}

/* Convert 16x16 raster-order pixels to MDEC block order (Y1,Y2,Y3,Y4).
 * Real PSX MDEC outputs via register reads in block order:
 *   Y1 (top-left 8x8), Y2 (top-right 8x8), Y3 (bot-left), Y4 (bot-right)
 * DMA1 rearranges automatically to raster order. */
static void raster_to_block_15(uint16_t *buf) {
    uint16_t tmp[256];
    memcpy(tmp, buf, 512);
    uint16_t *dst = buf;
    /* Y1: rows 0-7, cols 0-7 */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            *dst++ = tmp[y * 16 + x];
    /* Y2: rows 0-7, cols 8-15 */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            *dst++ = tmp[y * 16 + 8 + x];
    /* Y3: rows 8-15, cols 0-7 */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            *dst++ = tmp[(y + 8) * 16 + x];
    /* Y4: rows 8-15, cols 8-15 */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            *dst++ = tmp[(y + 8) * 16 + 8 + x];
}

/* Decode one macroblock into the non-DMA output FIFO */
static void mdec_decode_out_block(void) {
    int blk[DSIZE2 * 6];
    int depth = (mdec.reg0 >> 27) & 3;
    int signed_out = (mdec.reg0 >> 26) & 1;

    if (!mdec.rl || mdec.rl >= mdec.rl_end) {
        mdec.out_pos = 0;
        mdec.out_end = 0;
        return;
    }

    switch (depth) {
    case 0:
        mdec.rl = rl2blk_mono(blk, mdec.rl);
        y_to_mono4(blk, mdec.block_buffer, signed_out);
        mdec.out_end = SIZE_OF_4B_BLOCK;
        break;
    case 1:
        mdec.rl = rl2blk_mono(blk, mdec.rl);
        y_to_mono8(blk, mdec.block_buffer, signed_out);
        mdec.out_end = SIZE_OF_8B_BLOCK;
        break;
    case 2:
        mdec.rl = rl2blk(blk, mdec.rl);
        yuv2rgb24(blk, mdec.block_buffer);
        mdec.out_end = SIZE_OF_24B_BLOCK;
        break;
    default:
        mdec.rl = rl2blk(blk, mdec.rl);
        yuv2rgb15(blk, (uint16_t *)mdec.block_buffer);
        /* Register reads output block order (Y1,Y2,Y3,Y4) like real hardware */
        raster_to_block_15((uint16_t *)mdec.block_buffer);
        mdec.out_end = SIZE_OF_16B_BLOCK;
        break;
    }
    mdec.out_pos = 0;
}

uint32_t MDEC_ReadData(void) {
    /* If output FIFO is empty, try to decode next macroblock (manual mode) */
    if (mdec.out_pos >= mdec.out_end) {
        if (mdec.rl && mdec.rl < mdec.rl_end)
            mdec_decode_out_block();
        else
            return 0;
    }

    if (mdec.out_pos >= mdec.out_end)
        return 0;

    uint32_t val;
    memcpy(&val, &mdec.block_buffer[mdec.out_pos], 4);
    mdec.out_pos += 4;

    /* If all output consumed, clear busy */
    if (mdec.out_pos >= mdec.out_end && (!mdec.rl || mdec.rl >= mdec.rl_end))
        mdec.reg1 &= ~MDEC1_BUSY;

    return val;
}

void MDEC_WriteControl(uint32_t data) {
    if (data & MDEC1_RESET) {
        mdec.reg0 = 0;
        mdec.reg1 = 0;
        mdec.rl = NULL;
        mdec.rl_end = NULL;
        mdec.pending_dma1.adr = 0;
        mdec.block_buffer_pos = 0;
        mdec.out_pos = 0;
        mdec.out_end = 0;
        mdec.remaining_words = 0;
        mdec.fifo_state = MDEC_STATE_IDLE;
        mdec_in_write_idx = 0;
    }
}

/* Minimum halfwords of input before attempting a decode (simulate FIFO depth) */
#define MDEC_FIFO_THRESHOLD  64  /* 32 words */

/* Try to decode a macroblock if we have enough input and no output pending.
 * Only for manual (non-DMA) mode — DMA path handles decode in MDEC_DMA1. */
static void mdec_try_decode(void) {
    if (mdec.fifo_state != MDEC_STATE_RECV_DATA)
        return;  /* DMA mode: leave data for MDEC_DMA1 */
    if (mdec.out_pos < mdec.out_end)
        return;  /* output already available */
    if (!mdec.rl || mdec.rl >= mdec.rl_end)
        return;  /* no input data */
    int available = (int)(mdec.rl_end - mdec.rl);
    if (available < MDEC_FIFO_THRESHOLD && mdec.remaining_words > 0)
        return;  /* not enough input yet, and more is coming */
    mdec_decode_out_block();
}

uint32_t MDEC_ReadStatus(void) {
    /* Try to decode if conditions are met */
    mdec_try_decode();

    uint32_t v = mdec.reg1 & ~0xFFFF;  /* preserve upper bits, rebuild lower */

    int has_output = (mdec.out_pos < mdec.out_end) ||
                     (mdec.rl && mdec.rl < mdec.rl_end);

    /* Bit 31: Data-Out Fifo Empty */
    if (has_output)
        v &= ~0x80000000;
    else
        v |= 0x80000000;

    /* Bit 30: Data-In Fifo Full — signal when output is pending but unread */
    if (mdec.out_pos < mdec.out_end)
        v |= 0x40000000;
    else
        v &= ~0x40000000;

    /* Bits 28-27: DMA request flags */
    if (v & MDEC1_BUSY) {
        v |= 0x18000000;
    } else {
        v &= ~0x18000000;
    }

    /* Bits 26-25: depth, bit 24: signed, bit 23: bit15 — from command */
    int depth    = (mdec.reg0 >> 27) & 3;
    int sign_bit = (mdec.reg0 >> 26) & 1;
    int stp_bit  = (mdec.reg0 >> 25) & 1;
    v = (v & ~0x07800000) | (depth << 25) | (sign_bit << 24) | (stp_bit << 23);

    /* Bits 15-0: remaining parameter words minus 1 */
    if (mdec.remaining_words > 0)
        v |= ((mdec.remaining_words - 1) & 0xFFFF);
    else
        v |= 0xFFFF;

    return v;
}

/* ---------- DMA0: RAM → MDEC (compressed data input) ---------- */

void MDEC_DMA0(uint32_t adr, uint32_t bcr, uint32_t chcr) {
    uint32_t cmd = mdec.reg0;
    uint32_t block_size  = bcr & 0xFFFF;
    uint32_t block_count = (bcr >> 16) & 0xFFFF;
    if (block_count == 0) block_count = 1;
    if (block_size == 0)  block_size = 1;
    uint32_t total_words = block_size * block_count;

    if (chcr != 0x01000201) {
        printf("[MDEC] DMA0: unexpected chcr %08X\n", (unsigned)chcr);
        return;
    }

#ifdef ENABLE_VRAM_DUMP
    printf("[MDEC] DMA0: adr=%08X words=%u cmd=%08X\n", (unsigned)adr, (unsigned)total_words, (unsigned)cmd);
#endif

    mdec.reg1 |= MDEC1_STP;

    adr &= 0x1FFFFC;
    if (adr + total_words * 4 > PSX_RAM_SIZE) {
        printf("[MDEC] DMA0: bad madr %08X size %u\n", (unsigned)adr, (unsigned)total_words);
        return;
    }

    const void *mem = &psx_ram[adr];

    switch ((cmd >> 29) & 7) {
    case 1: { /* Decode macroblocks */
        mdec.reg1 |= MDEC1_BUSY;
        mdec.rl = (const uint16_t *)mem;
        mdec.rl_end = mdec.rl + total_words * 2;
        mdec.fifo_state = MDEC_STATE_IDLE;  /* DMA overrides manual input */
        mdec.out_pos = 0;
        mdec.out_end = 0;
        if (mdec.rl_end <= mdec.rl) break;

#ifdef ENABLE_VRAM_DUMP
        {
            static int rle_dump_done = 0;
            if (!rle_dump_done) {
                rle_dump_done = 1;
                /* Dump first 128 halfwords of RLE data */
                const uint16_t *p = mdec.rl;
                printf("[MDEC] RLE raw data (first 128 hw):\n");
                for (int j = 0; j < 128 && p + j < mdec.rl_end; j++) {
                    if (j % 16 == 0) printf("  [%3d]", j);
                    printf(" %04X", (unsigned)p[j]);
                    if (j % 16 == 15) printf("\n");
                }
                printf("\n");
            }
        }
#endif

        /* If a DMA1 request was pending, process it now */
        if (mdec.pending_dma1.adr) {
            MDEC_DMA1(mdec.pending_dma1.adr, mdec.pending_dma1.bcr,
                       mdec.pending_dma1.chcr);
        }
        mdec.pending_dma1.adr = 0;
        break;
    }
    case 2: { /* Upload quantization table */
        const uint8_t *p = (const uint8_t *)mem;
        iqtab_init(iq_y, p);
        if (total_words > 16)
            iqtab_init(iq_uv, p + 64);
        else
            iqtab_init(iq_uv, p); /* same table for UV */
        break;
    }
    case 3: { /* Upload IDCT scale table — transpose and store */
        const uint16_t *p = (const uint16_t *)mem;
        for (int sy = 0; sy < 8; sy++)
            for (int sx = 0; sx < 8; sx++)
                scale_table[sy * 8 + sx] = (int16_t)p[sx * 8 + sy];
        break;
    }
    default:
        printf("[MDEC] DMA0: unknown command %08X\n", (unsigned)cmd);
        break;
    }
    /* DMA completion is handled by dma.c (clears CHCR bit24, fires DICR) */
}

/* ---------- DMA1: MDEC → RAM (decoded output) ---------- */

void MDEC_DMA1(uint32_t adr, uint32_t bcr, uint32_t chcr) {
    uint32_t block_size  = bcr & 0xFFFF;
    uint32_t block_count = (bcr >> 16) & 0xFFFF;
    if (block_count == 0) block_count = 1;
    if (block_size == 0)  block_size = 1;
    uint32_t total_words = block_size * block_count;

    if (chcr != 0x01000200) {
        printf("[MDEC] DMA1: unexpected chcr %08X\n", (unsigned)chcr);
        return;
    }

#ifdef ENABLE_VRAM_DUMP
    printf("[MDEC] DMA1: adr=%08X words=%u depth=%d rl=%p rl_end=%p\n",
           (unsigned)adr, (unsigned)total_words, (mdec.reg0 >> 27) & 3,
           (void*)mdec.rl, (void*)mdec.rl_end);
#endif

    /* If MDEC not busy (no data to decode), defer */
    if (!(mdec.reg1 & MDEC1_BUSY) || !mdec.rl || mdec.rl >= mdec.rl_end) {
        mdec.pending_dma1.adr = adr;
        mdec.pending_dma1.bcr = bcr;
        mdec.pending_dma1.chcr = chcr;
        return;
    }

    adr &= 0x1FFFFC;
    uint32_t size = total_words * 4; /* bytes */

    if (adr + size > PSX_RAM_SIZE) {
        printf("[MDEC] DMA1: bad madr %08X size %u\n", (unsigned)adr, (unsigned)size);
        return;
    }

    uint8_t *image = &psx_ram[adr];
    int blk[DSIZE2 * 6];
    int depth = (mdec.reg0 >> 27) & 3;
    int signed_out = (mdec.reg0 >> 26) & 1;

    /* Select block size and decode function based on depth */
    uint32_t block_bytes;
    switch (depth) {
    case 0:  block_bytes = SIZE_OF_4B_BLOCK;  break;
    case 1:  block_bytes = SIZE_OF_8B_BLOCK;  break;
    case 2:  block_bytes = SIZE_OF_24B_BLOCK; break;
    default: block_bytes = SIZE_OF_16B_BLOCK; break;
    }

    /* Flush partial block from previous DMA1 */
    if (mdec.block_buffer_pos != 0) {
        int n = mdec.block_buffer + block_bytes - mdec.block_buffer_pos;
        if ((uint32_t)n > size) n = size;
        memcpy(image, mdec.block_buffer_pos, n);
        image += n;
        size -= n;
        mdec.block_buffer_pos = 0;
    }

    /* Decode macroblocks */
    while (size >= block_bytes && mdec.rl < mdec.rl_end) {
        if (depth <= 1) {
            mdec.rl = rl2blk_mono(blk, mdec.rl);
            if (depth == 0)
                y_to_mono4(blk, image, signed_out);
            else
                y_to_mono8(blk, image, signed_out);
        } else if (depth == 2) {
            mdec.rl = rl2blk(blk, mdec.rl);
            yuv2rgb24(blk, image);
        } else {
            mdec.rl = rl2blk(blk, mdec.rl);
            yuv2rgb15(blk, (uint16_t *)image);
        }
        image += block_bytes;
        size -= block_bytes;
    }

    /* Handle partial block at end of DMA */
    if (size != 0 && mdec.rl < mdec.rl_end) {
        if (depth <= 1) {
            mdec.rl = rl2blk_mono(blk, mdec.rl);
            if (depth == 0)
                y_to_mono4(blk, mdec.block_buffer, signed_out);
            else
                y_to_mono8(blk, mdec.block_buffer, signed_out);
        } else if (depth == 2) {
            mdec.rl = rl2blk(blk, mdec.rl);
            yuv2rgb24(blk, mdec.block_buffer);
        } else {
            mdec.rl = rl2blk(blk, mdec.rl);
            yuv2rgb15(blk, (uint16_t *)mdec.block_buffer);
        }
        memcpy(image, mdec.block_buffer, size);
        mdec.block_buffer_pos = mdec.block_buffer + size;
    }

    /* Invalidate JIT pages that were written */
    uint32_t start_page = (adr >> 12);
    uint32_t end_page   = ((adr + total_words * 4 - 1) >> 12);
    for (uint32_t p = start_page; p <= end_page && p < (PSX_RAM_SIZE >> 12); p++)
        jit_invalidate_page(p);

    /* Clear busy only when all RLE data has been consumed */
    if (!mdec.rl || mdec.rl >= mdec.rl_end)
        mdec.reg1 &= ~MDEC1_BUSY;

    /* Timing: defer completion for realistic bus timing */
    /* For now, complete immediately — timing refinement in Phase 2 */
}

void MDEC_DMA1_Complete(void) {
    /* Placeholder for scheduler-based deferred completion */
}
