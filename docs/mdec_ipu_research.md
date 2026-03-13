# MDEC ↔ PS2 IPU Research & Implementation Strategy

## 1. PSX MDEC (Motion Decoder) — Complete Reference

### Registers

| Address      | R/W   | Name           | Description |
|-------------|-------|----------------|-------------|
| 0x1F801820  | Write | Command/Param  | MDEC command word or data parameter |
| 0x1F801820  | Read  | Data/Response  | Decompressed output data |
| 0x1F801824  | Read  | Status         | Decoder status (see below) |
| 0x1F801824  | Write | Control/Reset  | bit31=Reset, bit30=EnableDMAIn, bit29=EnableDMAOut |

### Status Register (0x1F801824 Read)

| Bits    | Field                    |
|---------|--------------------------|
| 31      | Data-Out FIFO Empty      |
| 30      | Data-In FIFO Full        |
| 29      | Command Busy             |
| 28      | Data-In Request (DMA)    |
| 27      | Data-Out Request (DMA)   |
| 26-25   | Output Depth (0=4bit, 1=8bit, 2=24bit, 3=15bit) |
| 24      | Signed output (±128)     |
| 23      | Bit15 (set bit15 in 15bit output) |
| 18-16   | Current Block (0-5)      |
| 15-0    | Remaining param words-1  |

### Commands

| Cmd | Name                 | Parameters |
|-----|----------------------|------------|
| 1   | Decode Macroblocks   | bits[28:27]=depth, bit26=signed, bit25=bit15, bits[15:0]=param_words |
| 2   | Set Quant Table      | 16 words (Y table), optionally +16 words (UV table if bit0 set) |
| 3   | Set Scale Table      | 32 words (64 × 16-bit scale coefficients) |

### DMA Channels

- **DMA0 (MDEC.In)**: RAM → MDEC, block size = 0x20 words, SyncMode=1
- **DMA1 (MDEC.Out)**: MDEC → RAM, block size = 0x20 words, SyncMode=1

### Decoding Pipeline

```
Compressed data (RLE-encoded DCT coefficients)
    │
    ▼
┌─────────────┐
│ RLE Decode   │  Parse (q_scale, DC) + (skip, AC) halfwords
│              │  End-of-block = 0xFE00
└─────┬───────┘
      │  64 dequantized coefficients
      ▼
┌─────────────┐
│ Zigzag      │  Reorder from scan order to 8×8 spatial
│ Reorder     │  (zagzig[64] or zigzag[64] table)
└─────┬───────┘
      │  8×8 block (s16)
      ▼
┌─────────────┐
│ IDCT        │  2D Inverse DCT (rows then columns)
│ (8×8)       │  Uses programmable scale table (Cmd 3)
└─────┬───────┘
      │  8×8 block (s16, clamped to -128..127)
      ▼
┌─────────────┐
│ YUV→RGB     │  For colored: Cr,Cb,Y1,Y2,Y3,Y4 → 16×16 RGB
│ or Y→Mono   │  For mono: Y → 8×8 grayscale
└─────┬───────┘
      │  Output: 4bit, 8bit, 15bit, or 24bit pixels
      ▼
DMA1 → RAM
```

### Macroblock Structure

**Colored macroblock** = 6 blocks → 16×16 pixel output:
- Block 0: Cr (chrominance red, 8×8, shared across 16×16)
- Block 1: Cb (chrominance blue, 8×8, shared across 16×16)
- Block 2: Y1 (luminance, top-left 8×8)
- Block 3: Y2 (luminance, top-right 8×8)
- Block 4: Y3 (luminance, bottom-left 8×8)
- Block 5: Y4 (luminance, bottom-right 8×8)

**Mono macroblock** = 1 block → 8×8 pixel output:
- Block 0: Y (luminance only)

### RLE Encoding Format

First halfword per block: `(q_scale << 10) | (DC_value & 0x3FF)`
Subsequent halfwords: `(skip_count << 10) | (AC_value & 0x3FF)`
End-of-block marker: `0xFE00`

**Dequantization formula:**
```
if (q_scale == 0):
    val = sign_extend_10(raw) * 2
else:
    val = (sign_extend_10(raw) * quant_table[k] * q_scale + 4) / 8
val = clamp(val, -0x400, 0x3FF)
```

### IDCT

Standard 2D IDCT using programmable 64-entry scale table (set via Cmd 3):
```
for each row x:
    for each column y:
        sum = Σ(blk[u*8+x] * scale_table[y*8+u]) for u=0..7
        temp[x + y*8] = sum
for each row x:
    for each column y:
        sum = Σ(temp[u + y*8] * scale_table[x*8+u]) for u=0..7
        blk[x + y*8] = clamp(sign_extend_9((sum >> 32) + round), -128, 127)
```

### YUV→RGB Conversion

```
R = Y + 1.402 * Cr
G = Y - 0.3437 * Cb - 0.7143 * Cr  
B = Y + 1.772 * Cb
Output = clamp(value, -128, 127) + (signed ? 0 : 128)
```

---

## 2. PS2 IPU (Image Processing Unit) — Relevant Capabilities

### Registers

| Address      | R/W  | Name       | Description |
|-------------|------|------------|-------------|
| 0x10002000  | W/R  | IPU_CMD    | Send command (W) / Read result (R), bit63=Busy |
| 0x10002010  | R/W  | IPU_CTRL   | Control/status (IFC, OFC, CBP, ECD, SCD, IDP, AS, IVF, QST, MP1, PCT, RST, Busy) |
| 0x10002020  | R    | IPU_BP     | Bitstream position (IFC, FP, BP) |
| 0x10002030  | R    | IPU_TOP    | Top 32 bits of bitstream |

### FIFOs

| Address      | Dir  | Name      | Size     |
|-------------|------|-----------|----------|
| 0x10007000  | Read | Out FIFO  | 8 QWords |
| 0x10007010  | Write| In FIFO   | 8 QWords |

### DMA Channels

- **IPU_TO** (Ch4): EE RAM → IPU input FIFO
- **IPU_FROM** (Ch3): IPU output FIFO → EE RAM

### IPU Commands

| Code | Name   | Description |
|------|--------|-------------|
| 0x00 | BCLR   | Clear input FIFO, reset bitstream pointer |
| 0x01 | IDEC   | Intra-frame slice decode → RGB32/RGB16 output |
| 0x02 | BDEC   | Macroblock decode (intra/non-intra, IDCT included) |
| 0x03 | VDEC   | VLC decode (MB address increment, MB type, motion code) |
| 0x04 | FDEC   | Fixed-length bitstream decode |
| 0x05 | SETIQ  | Set quantization table (intra or non-intra) |
| 0x06 | SETVQ  | Set VQ color lookup table (16 entries) |
| 0x07 | CSC    | Color space conversion YCbCr → RGB32/RGB16 |
| 0x08 | PACK   | Pixel format conversion (RGB32 → 4bit/16bit) |
| 0x09 | SETTH  | Set alpha thresholds for PACK |

### Key IPU_CTRL Bits

- **MP1** (bit23): MPEG1 mode (vs MPEG2) — PSX MDEC is MPEG1-like
- **IDP** (bits23:22): Intra DC precision
- **QST** (bit21): Quantizer scale type
- **IVF** (bit20): Intra VLC format
- **AS** (bit19): Alternate scan
- **PCT** (bits25:24): Picture coding type (I/P/B/D)
- **RST** (bit30): Software reset
- **Busy** (bit31): Command busy

### IDEC Command Details

```
IDEC: bits[31:28]=0x1
  bit25: SGN — when set, output RGB decremented by 128 per channel (matches MDEC signed!)
  bit24: DTE — dither enable
  bit23: OFM — output format (0=RGB32, 1=RGB16)
  bit22: DTD — decode DT flag
  bits[20:16]: QSC — quantizer scale code
  bits[3:0]: FB — FIFO advance count (skip bits)
```

### CSC Command Details

```
CSC: bits[31:28]=0x7
  bit27: OFM — output format (0=RGB32, 1=RGB16)
  bit26: DTE — dither enable
  bits[15:0]: MBC — macroblock count
```
Converts YCbCr macroblocks to RGB. Input: 384 bytes per macroblock (6 × 64 bytes = Cr+Cb+Y1+Y2+Y3+Y4). Output: 16×16 RGB32 or RGB16.

### SETIQ Command

```
SETIQ: bits[31:28]=0x5
  bit27: IQM — 0=intra table, 1=non-intra table
```
Loads 64-byte quantization matrix. This is analogous to MDEC Cmd 2!

---

## 3. Compatibility Analysis: MDEC vs IPU

### What Matches

| Feature              | MDEC                    | IPU                      | Compatible? |
|---------------------|------------------------|--------------------------|-------------|
| Quantization tables | 64-byte tables (Y/UV)  | SETIQ loads 64-byte table | ✅ Same size/concept |
| IDCT                | Custom scale table     | Standard MPEG1/2 IDCT    | ⚠️ Different! |
| Color conversion    | YUV→RGB with ±128      | CSC YCbCr→RGB, SGN flag  | ✅ Very close |
| Output formats      | 4/8/15/24 bit          | RGB32/RGB16 + PACK       | ✅ Partial match |
| Signed mode         | ±128 offset            | SGN bit in IDEC          | ✅ Exact match |

### Critical Incompatibilities

1. **RLE vs VLC encoding**: MDEC uses simple RLE (skip+value halfwords), IPU expects Huffman VLC-encoded MPEG bitstreams. **Cannot use IPU to decode MDEC's compressed data directly.**

2. **IDCT scale table**: MDEC uses a programmable 64-entry scale table (set by Cmd 3). IPU uses the standard MPEG IDCT with fixed cosine coefficients. If a game uses the standard MDEC scale table (which is the standard DCT matrix), the results would match. If a game uses a custom scale table, they won't match. **Most games use the standard table.**

3. **Dequantization**: MDEC's dequant formula differs slightly from standard MPEG1. The `+4)/8` rounding and the q_scale=0 special case are MDEC-specific.

4. **Block ordering**: MDEC orders blocks as Cr, Cb, Y1-Y4. MPEG standard is Y1-Y4, Cb, Cr. The IPU follows standard MPEG ordering.

---

## 4. Implementation Strategy — IPU Hardware Decode (Approach B)

### Chosen Approach: MDEC RLE → MPEG-2 VLC Re-encode (MMI) → IPU IDEC

Offload IDCT + YCbCr→RGB to PS2's dedicated IPU hardware. The EE CPU only parses
the MDEC RLE data and re-encodes it as an MPEG-2 VLC bitstream using MMI SIMD.
The IPU then handles VLC decode → dequant → IDCT → CSC entirely in hardware.

**Key reference**: ps2sdk `ee/mpeg/src/libmpeg_core.c` — production IPU programming.

### Data Flow

```
PSX Game → DMA ch0 → MDEC RLE data (halfwords)
                          │
                    ┌─────▼──────┐
                    │ EE (MMI)   │  1. Parse RLE → (run, level) pairs
                    │ Re-encode  │  2. Reorder blocks: MDEC(Cr,Cb,Y1-4) → MPEG(Y1-4,Cb,Cr)
                    │ to MPEG-2  │  3. Encode as MPEG-2 VLC bitstream
                    │ VLC        │  4. Uses lookup table + 12-bit escape codes
                    └─────┬──────┘
                          │  VLC bitstream (compact)
                    ┌─────▼──────┐
                    │ EE DMA ch4 │  D4 (toIPU): push bitstream to IPU input FIFO
                    └─────┬──────┘
                          │
                    ┌─────▼──────┐
                    │   PS2 IPU  │  IDEC command (all in dedicated hardware):
                    │ (hardware) │  VLC decode → dequant → IDCT → CSC → RGB
                    │            │  EE is FREE while IPU works
                    └─────┬──────┘
                          │  RGB32 / RGB16 pixels
                    ┌─────▼──────┐
                    │ EE DMA ch3 │  D3 (fromIPU): receive decoded pixels
                    └─────┬──────┘
                          │
                    PSX MDEC output buffer ← DMA ch1 readback
```

### VLC Re-encoding Details

MPEG-2 VLC encoding for each DCT coefficient (run, level) pair:

**Standard entries** (Table B.14): Common combinations (run=0..|level|=1..40, etc.)
map to variable-length Huffman codes (2-16 bits). ~70 entries in table.

**Escape code** (everything else):
```
Format: 000001 (6 bits) + run (6 bits) + level_signed (12 bits) = 24 bits
12-bit signed range: [-2048, +2047]
→ Covers MDEC's 10-bit signed range [-512, +511] perfectly ✓
```

**Requires IPU in MPEG-2 mode** (MP1=0 in IPU_CTRL). MPEG-1 mode only has
8-bit escape codes [-255, +255] which can't cover MDEC's full range.

**Implementation**: Lookup table `vlc_table[run][|level|]` → {code, length}
- For entries in table: emit compact VLC code
- For others: emit 24-bit escape code
- End of block: emit '10' (2 bits)
- MMI accelerates: process 8 coefficients per iteration using 128-bit ops

### Quantization Matching IPU ↔ MDEC

**Problem**: Dequantization formulas differ:
```
MDEC:   val = (level × qt[k] × q_scale + 4) >> 3   (÷8, rounded)
MPEG-2: val = (level × W[k] × quantiser_scale) / 16 (÷16, truncated)
```

**Solution**: Set IPU quant table W[k] = 2 × MDEC_qt[k]:
```
IPU:  (level × 2×qt[k] × qs) / 16 = (level × qt[k] × qs) / 8
MDEC: (level × qt[k] × qs + 4) / 8
Difference: ±1 from rounding — imperceptible in FMV video
```

Load via SETIQ command (same as ps2sdk `_MPEG_SetDefQM()`).

### Block Reordering

MDEC block order: Cr, Cb, Y1, Y2, Y3, Y4
MPEG block order: Y1, Y2, Y3, Y4, Cb, Cr

Reorder during VLC encoding — emit blocks 2,3,4,5,1,0 from MDEC input.

### IPU Programming (from ps2sdk libmpeg patterns)

```c
// === One-time init ===
*IPU_CTRL = IPU_RST;                // Reset IPU
*IPU_CMD  = IPU_BCLR;              // Clear input FIFO
// IPU_CTRL config:
//   MP1=0 (MPEG-2 → 12-bit escape), IVF=0 (Table B.14),
//   QST=0 (linear scale), AS=0 (standard zigzag)

// === Per-frame: load quant table ===
*IPU_CMD = IPU_SETIQ | 0;          // Set intra quant table
// DMA 64 bytes (2× MDEC qt_y) to IPU FIFO via D4

// === Per-macroblock ===
*IPU_CMD = IPU_BCLR;               // Clear FIFO

// Push VLC bitstream to IPU
*D4_MADR = (u32)vlc_buffer;
*D4_QWC  = vlc_size_qw;
*D4_CHCR = 0x101;                  // Start mem→IPU

// Issue IDEC: VLC decode + dequant + IDCT + CSC in ONE command
*IPU_CMD = IPU_IDEC
         | (sgn << 25)             // Match MDEC signed output flag
         | (ofm << 23)             // 0=RGB32, 1=RGB16
         | (qsc << 16);           // Quantizer scale from MDEC data

// Receive decoded RGB pixels
*D3_MADR = (u32)rgb_output;
*D3_QWC  = 64;                     // 16×16×4 bytes = 64 QW (RGB32)
*D3_CHCR = 0x100;                  // Start IPU→mem
```

### MMI SIMD for VLC Encoding

PS2 EE MMI instructions for parallel processing:
- **PEXTH/PEXTLH**: Extract/pack halfwords (unpack MDEC RLE data)
- **PSLLH/PSRLH**: 128-bit shift on 8×16-bit (bitstream packing)
- **PMAXH/PMINH**: Parallel min/max for clamping
- **PADDH/PSUBH**: Parallel add/sub for arithmetic
- **POR/PAND**: Bitwise ops for bitstream construction
- Process 8 RLE halfwords per iteration → 8× throughput vs scalar

### Performance Estimate (320×240 frame, 300 macroblocks)

| Stage | Software-only | Approach B (MMI+IPU) |
|-------|:------------:|:--------------------:|
| RLE parse | 115K cycles | 58K cycles |
| Dequant | 115K cycles | 115K cycles |
| VLC encode | — | ~45K cycles (MMI) |
| DMA setup | — | ~6K cycles |
| IDCT | **922K cycles** | **0 (IPU HW)** |
| YUV→RGB | **768K cycles** | **0 (IPU HW)** |
| **Total EE** | **~1.92M (6.5ms)** | **~224K (0.76ms)** |
| **EE savings** | — | **~88%** |

### Alternative Approaches (for reference)

#### Alt-A: Software MDEC + IPU CSC Only

Software RLE+dequant+IDCT → IPU CSC for YCbCr→RGB only.
Saves ~30% (only color conversion in HW). Simpler but less benefit.

#### Alt-B: VU0 IDCT

Use VU0 vector math for IDCT butterfly (we already have VU0 infra from GTE).
Could complement Approach B if IPU path has issues.

### DMA Channel Implementation

The DMA code in `dma.c` currently ignores channels 0 and 1. Need to add:

```c
// In DMA_Write(), channel trigger section:
else if (ch == 0)
    MDEC_DMA0(dma_channels[ch].madr, dma_channels[ch].bcr, dma_channels[ch].chcr);
else if (ch == 1)
    MDEC_DMA1(dma_channels[ch].madr, dma_channels[ch].bcr, dma_channels[ch].chcr);
```

**DMA0 (MDEC.In)**: Direction = to device (bit0=1), SyncMode=1 (blocks)
- Typically: block_size=0x20 words, block_count=N
- Reads words from RAM, writes to MDEC input FIFO
- First word of a transfer is the command word

**DMA1 (MDEC.Out)**: Direction = from device (bit0=0), SyncMode=1 (blocks)
- Typically: block_size=0x20 words, block_count=N
- Reads decoded pixels from MDEC output, writes to RAM
- Must trigger DMA request only when output data is available

### MDEC State Machine

```
States:
  Idle                — Waiting for command
  ReceivingData       — DMA0 feeding compressed data
  Processing          — VLC re-encode + IPU decode in progress
  OutputReady         — IPU done, RGB pixels ready for DMA1
  SetIqTable          — Loading quantization table
  SetScaleTable       — Loading IDCT scale table (ignored for IPU path)

DMA0 flow:
  1. Game triggers DMA0 (CHCR bit24)
  2. Copy RLE data from RAM to internal buffer
  3. Parse command word → dispatch (decode / set_qt / set_scale)

Decode flow (Approach B):
  1. Parse all macroblocks from RLE input
  2. For each macroblock:
    a. Extract (run, level) pairs from 6 blocks
    b. Reorder blocks MDEC→MPEG
    c. VLC-encode using MMI → bitstream buffer
  3. Configure IPU (SETIQ with 2× MDEC qt)
  4. Start IPU IDEC via DMA ch4/ch3
  5. When IPU done → mark OutputReady
  6. DMA1 reads from output buffer

Timing:
  MDEC_BIAS = 14 cycles per byte (from PCSX-ReARMed)
  Colored macroblock output: 16×16×2 = 512 bytes (15-bit) or 16×16×3 = 768 bytes (24-bit)
```

### Key Data Structures

```c
typedef struct {
    /* Registers */
    uint32_t reg0;                  /* Command/param register */
    uint32_t reg1;                  /* Status register */

    /* Quantization tables */
    uint8_t iq_y[64];              /* Luminance quant table */
    uint8_t iq_uv[64];            /* Chrominance quant table */

    /* IPU-adjusted quant table (2× MDEC for IPU dequant matching) */
    uint8_t ipu_qt[64];

    /* Input buffer (RLE compressed data from DMA0) */
    const uint16_t *rl;            /* Current read pointer */
    const uint16_t *rl_end;        /* End of input data */

    /* VLC bitstream buffer (re-encoded for IPU) */
    uint8_t vlc_buffer[16384] __attribute__((aligned(16)));
    uint32_t vlc_size;             /* Bytes written */

    /* Output buffer (RGB from IPU) */
    uint32_t rgb_output[256 * 64] __attribute__((aligned(16)));
    /* Max ~64 macroblocks batched, 256 words (RGB32) each */

    /* Output state for DMA1 readback */
    uint8_t *out_ptr;
    uint32_t out_remaining;

    /* Pending DMA1 */
    struct { uint32_t adr, bcr, chcr; } pending_dma1;

    /* Flags from command */
    int rgb24;                     /* 1=24bit, 0=15bit output */
    int signed_output;             /* 1=signed (±128) */
    int stp_bit;                   /* Set bit15 in 15-bit output */
} MDEC_State;
```

### VLC Lookup Table Design

```c
/* MPEG-2 Table B.14 encoded entries */
typedef struct {
    uint16_t code;                 /* VLC code bits (left-aligned) */
    uint8_t  length;              /* Number of bits (2-16) */
} VLC_Entry;

/* Lookup: vlc_table_b14[run][level] for run=0..31, |level|=1..40 */
static const VLC_Entry vlc_table_b14[32][41];

/* Escape code builder */
static inline uint32_t vlc_escape(uint8_t run, int16_t level) {
    /* 000001 (6) + run (6) + level (12) = 24 bits */
    return (0x01 << 18) | ((run & 0x3F) << 12) | (level & 0xFFF);
}

/* End-of-block = '10' (2 bits) */
#define VLC_EOB_CODE  0x2
#define VLC_EOB_BITS  2
```

### Files to Create/Modify

| File | Action | Description |
|------|--------|-------------|
| `src/mdec.h` | Create | MDEC API + VLC table declarations |
| `src/mdec.c` | Create | MDEC state machine, RLE parse, VLC encode, IPU interface |
| `src/mdec_vlc_table.c` | Create | Pre-built MPEG-2 Table B.14 VLC lookup |
| `src/hardware.c` | Modify | Wire MDEC reads/writes to 0x1F801820/24 |
| `src/dma.c` | Modify | Add DMA channel 0/1 handlers |

### Test Strategy

1. **Unit test VLC encoder**: Encode known MDEC blocks → verify IPU decodes correctly
2. **Known FMV games**: Crash Bandicoot intro (Naughty Dog logo), FF7, MK2 cutscenes
3. **PCSX-ReARMed reference**: Compare MDEC output frame-by-frame
4. **IPU sanity**: Feed known MPEG bitstream → verify IPU output matches expected

---

## 5. Implementation Plan

### Phase 1: Software MDEC (get FMVs working)

Even with Approach B as the target, a software MDEC fallback is needed for:
- Initial bring-up and testing
- Edge cases where IPU re-encoding might fail
- Debugging reference

1. Create `mdec.c/h` with register I/O and state machine
2. Wire into `hardware.c` (0x1F801820/24)
3. Wire DMA channels 0/1 in `dma.c`
4. Implement Cmd 2 (Set Quant Table)
5. Implement Cmd 1 (Decode Macroblock) — full software: RLE → dequant → IDCT → YUV→RGB
6. Test with Crash Bandicoot FMV

### Phase 2: IPU Hardware Decode (Approach B)

1. Build MPEG-2 VLC lookup table (Table B.14)
2. Implement VLC encoder (scalar first, then MMI)
3. IPU programming: SETIQ, BCLR, IDEC, DMA ch3/ch4 setup
4. Replace software IDCT+YUV→RGB with IPU path
5. Batch macroblocks for DMA efficiency
6. Test: same games, compare output quality

### Phase 3: MMI Optimization

1. MMI-accelerated RLE parsing (8 halfwords per iteration)
2. MMI-accelerated VLC encoding (parallel table lookup + bitstream packing)
3. Profile: measure EE cycles per frame with/without MMI

---

## 6. Key References

| Source | Content | Location |
|--------|---------|----------|
| ps2sdk libmpeg | IPU programming: BDEC, CSC, SETIQ, DMA | `ee/mpeg/src/libmpeg_core.c` |
| ps2sdk libmpeg | MPEG decode flow using IPU | `ee/mpeg/src/libmpeg.c` |
| ps2sdk dma.h | DMA channel defines (ch3=fromIPU, ch4=toIPU) | `ee/dma/include/dma.h` |
| PCSX-ReARMed | Complete MDEC: RLE, IDCT, YUV→RGB, DMA0/1 | `libpcsxcore/mdec.c` |
| DuckStation | MDEC state machine, timing, formats | `src/core/mdec.cpp` |
| ps2tek | IPU registers, commands, FIFO, DMA | psi-rockin.github.io/ps2tek |
| psx-spx | MDEC registers, RLE format, IDCT, DMA | nocash PSX specs |
