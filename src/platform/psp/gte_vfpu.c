/**
 * gte_vfpu.c — VFPU-accelerated GTE for PSP
 *
 * Uses the PSP's VFPU (Vector Floating-Point Unit) to accelerate the
 * GTE matrix-vector multiply (MVMVA core), which is the bottleneck for
 * RTPS/RTPT, NCS/NCT, NCDS/NCDT, and all lighting operations.
 *
 * Same approach as the PS2 VU0 path in gte.c:
 *   - Pre-scale matrix elements by 1/4096 (sf=1 assumed)
 *   - Multiply in float32 via VFPU vtfm3.t instruction
 *   - Add translation vector
 *   - Convert result back to int32 for MAC1/2/3
 *   - Saturation/flags computed exactly in C (same as VU0 path)
 *
 * Precision: float32 has 24-bit mantissa. GTE products are 16x16 = up to
 * 30 bits. Max error ~64 per product, ~192 for a 3-term dot product.
 * For typical game values (small matrix x small vertex), error is <=1.
 *
 * VFPU register allocation per multiply:
 *   M000: 3x3 matrix (columns loaded via lv.q C000/C010/C020)
 *   C100: input vertex
 *   C200: translation vector
 *   C300: result
 */
#include "superpsx.h"

/* ---- Short names for GTE registers (matching gte.c) ---- */
#define D(n) cpu->cp2_data[(n)]
#define C(n) cpu->cp2_ctrl[(n)]

/* Data register indices */
enum {
    vd_VXY0 = 0, vd_VZ0 = 1, vd_VXY1 = 2, vd_VZ1 = 3,
    vd_VXY2 = 4, vd_VZ2 = 5,
    vd_IR1 = 9, vd_IR2 = 10, vd_IR3 = 11,
    vd_MAC1 = 25, vd_MAC2 = 26, vd_MAC3 = 27,
};

/* Control register indices */
enum {
    vc_RT11RT12 = 0, vc_RT13RT21 = 1, vc_RT22RT23 = 2,
    vc_RT31RT32 = 3, vc_RT33 = 4,
    vc_TRX = 5, vc_TRY = 6, vc_TRZ = 7,
    vc_L11L12 = 8, vc_L13L21 = 9, vc_L22L23 = 10,
    vc_L31L32 = 11, vc_L33 = 12,
    vc_RBK = 13, vc_GBK = 14, vc_BBK = 15,
    vc_LR1LR2 = 16, vc_LR3LG1 = 17, vc_LG2LG3 = 18,
    vc_LB1LB2 = 19, vc_LB3 = 20,
};

static inline int16_t lo16v(uint32_t v) { return (int16_t)(v & 0xFFFF); }
static inline int16_t hi16v(uint32_t v) { return (int16_t)(v >> 16); }

/* Local saturate_ir (same logic as gte.c, duplicated to avoid
 * de-inlining the static version in gte.c) */
extern uint32_t gte_flag_vfpu; /* set by gte.c before calling VFPU path */

static inline int32_t vfpu_saturate_ir(int32_t val, int n, int lm)
{
    int32_t lo = lm ? 0 : -0x8000;
    if (val < lo || val > 0x7FFF)
        gte_flag_vfpu |= (1u << (24 + 1 - n));
    if (val < lo) return lo;
    if (val > 0x7FFF) return 0x7FFF;
    return val;
}

/* ================================================================
 * Matrix float cache — ROW-major layout for VFPU vtfm3.t
 *
 * Unlike PS2 VU0 (vmulax/vmadday/vmaddz broadcast = column-major),
 * vtfm3.t reads M000 COLUMNS for dot products:
 *   result[i] = dot(Column_i(M000), Vector)
 *
 * So we load each GTE matrix ROW into a VFPU column:
 *   lv.q C000 = row1 = { r11/s, r12/s, r13/s, 0 }
 *   lv.q C010 = row2 = { r21/s, r22/s, r23/s, 0 }
 *   lv.q C020 = row3 = { r31/s, r32/s, r33/s, 0 }
 *
 * Then vtfm3.t C300, M000, C100 gives:
 *   result[0] = dot(C000, V) = r11*VX + r12*VY + r13*VZ
 *   result[1] = dot(C010, V) = r21*VX + r22*VY + r23*VZ
 *   result[2] = dot(C020, V) = r31*VX + r32*VY + r33*VZ
 *
 * Pre-scaled by 1/4096 so float result = MAC value (for sf=1).
 * ================================================================ */

/* RT matrix: ctrl[0..4], translation: ctrl[5..7] */
/* row[i] holds GTE matrix row i: {ri1/s, ri2/s, ri3/s, 0} */
float vfpu_rt_row1[4] __attribute__((aligned(16)));
float vfpu_rt_row2[4] __attribute__((aligned(16)));
float vfpu_rt_row3[4] __attribute__((aligned(16)));
float vfpu_rt_trans[4] __attribute__((aligned(16)));
static uint32_t vfpu_rt_snap[8];

void vfpu_refresh_rt_matrix(R3000CPU *cpu)
{
    int16_t r11 = lo16v(C(vc_RT11RT12)), r12 = hi16v(C(vc_RT11RT12));
    int16_t r13 = lo16v(C(vc_RT13RT21)), r21 = hi16v(C(vc_RT13RT21));
    int16_t r22 = lo16v(C(vc_RT22RT23)), r23 = hi16v(C(vc_RT22RT23));
    int16_t r31 = lo16v(C(vc_RT31RT32)), r32 = hi16v(C(vc_RT31RT32));
    int16_t r33 = lo16v(C(vc_RT33));

    const float s = 1.0f / 4096.0f;
    /* Row-major: row[i] = { ri1/s, ri2/s, ri3/s, 0 } */
    vfpu_rt_row1[0] = (float)r11 * s;
    vfpu_rt_row1[1] = (float)r12 * s;
    vfpu_rt_row1[2] = (float)r13 * s;
    vfpu_rt_row1[3] = 0.0f;
    vfpu_rt_row2[0] = (float)r21 * s;
    vfpu_rt_row2[1] = (float)r22 * s;
    vfpu_rt_row2[2] = (float)r23 * s;
    vfpu_rt_row2[3] = 0.0f;
    vfpu_rt_row3[0] = (float)r31 * s;
    vfpu_rt_row3[1] = (float)r32 * s;
    vfpu_rt_row3[2] = (float)r33 * s;
    vfpu_rt_row3[3] = 0.0f;

    vfpu_rt_trans[0] = (float)(int32_t)C(vc_TRX);
    vfpu_rt_trans[1] = (float)(int32_t)C(vc_TRY);
    vfpu_rt_trans[2] = (float)(int32_t)C(vc_TRZ);
    vfpu_rt_trans[3] = 0.0f;

    for (int i = 0; i < 8; i++)
        vfpu_rt_snap[i] = C(i);
}

int vfpu_rt_is_dirty(R3000CPU *cpu)
{
    for (int i = 0; i < 8; i++)
        if (vfpu_rt_snap[i] != C(i))
            return 1;
    return 0;
}

/* Light matrix: ctrl[8..12] */
float vfpu_lt_row1[4] __attribute__((aligned(16)));
float vfpu_lt_row2[4] __attribute__((aligned(16)));
float vfpu_lt_row3[4] __attribute__((aligned(16)));
static uint32_t vfpu_lt_snap[5];

void vfpu_refresh_lt_matrix(R3000CPU *cpu)
{
    int16_t l11 = lo16v(C(vc_L11L12)), l12 = hi16v(C(vc_L11L12));
    int16_t l13 = lo16v(C(vc_L13L21)), l21 = hi16v(C(vc_L13L21));
    int16_t l22 = lo16v(C(vc_L22L23)), l23 = hi16v(C(vc_L22L23));
    int16_t l31 = lo16v(C(vc_L31L32)), l32 = hi16v(C(vc_L31L32));
    int16_t l33 = lo16v(C(vc_L33));

    const float s = 1.0f / 4096.0f;
    vfpu_lt_row1[0] = (float)l11 * s;
    vfpu_lt_row1[1] = (float)l12 * s;
    vfpu_lt_row1[2] = (float)l13 * s;
    vfpu_lt_row1[3] = 0.0f;
    vfpu_lt_row2[0] = (float)l21 * s;
    vfpu_lt_row2[1] = (float)l22 * s;
    vfpu_lt_row2[2] = (float)l23 * s;
    vfpu_lt_row2[3] = 0.0f;
    vfpu_lt_row3[0] = (float)l31 * s;
    vfpu_lt_row3[1] = (float)l32 * s;
    vfpu_lt_row3[2] = (float)l33 * s;
    vfpu_lt_row3[3] = 0.0f;

    for (int i = 0; i < 5; i++)
        vfpu_lt_snap[i] = C(8 + i);
}

int vfpu_lt_is_dirty(R3000CPU *cpu)
{
    for (int i = 0; i < 5; i++)
        if (vfpu_lt_snap[i] != C(8 + i))
            return 1;
    return 0;
}

/* Color matrix: ctrl[16..20] */
float vfpu_lc_row1[4] __attribute__((aligned(16)));
float vfpu_lc_row2[4] __attribute__((aligned(16)));
float vfpu_lc_row3[4] __attribute__((aligned(16)));
static uint32_t vfpu_lc_snap[5];

void vfpu_refresh_lc_matrix(R3000CPU *cpu)
{
    int16_t lr1 = lo16v(C(vc_LR1LR2)), lr2 = hi16v(C(vc_LR1LR2));
    int16_t lr3 = lo16v(C(vc_LR3LG1)), lg1 = hi16v(C(vc_LR3LG1));
    int16_t lg2 = lo16v(C(vc_LG2LG3)), lg3 = hi16v(C(vc_LG2LG3));
    int16_t lb1 = lo16v(C(vc_LB1LB2)), lb2 = hi16v(C(vc_LB1LB2));
    int16_t lb3 = lo16v(C(vc_LB3));

    const float s = 1.0f / 4096.0f;
    vfpu_lc_row1[0] = (float)lr1 * s;
    vfpu_lc_row1[1] = (float)lr2 * s;
    vfpu_lc_row1[2] = (float)lr3 * s;
    vfpu_lc_row1[3] = 0.0f;
    vfpu_lc_row2[0] = (float)lg1 * s;
    vfpu_lc_row2[1] = (float)lg2 * s;
    vfpu_lc_row2[2] = (float)lg3 * s;
    vfpu_lc_row2[3] = 0.0f;
    vfpu_lc_row3[0] = (float)lb1 * s;
    vfpu_lc_row3[1] = (float)lb2 * s;
    vfpu_lc_row3[2] = (float)lb3 * s;
    vfpu_lc_row3[3] = 0.0f;

    for (int i = 0; i < 5; i++)
        vfpu_lc_snap[i] = C(16 + i);
}

int vfpu_lc_is_dirty(R3000CPU *cpu)
{
    for (int i = 0; i < 5; i++)
        if (vfpu_lc_snap[i] != C(16 + i))
            return 1;
    return 0;
}

/* BK translation: ctrl[13..15] */
float vfpu_bk_trans[4] __attribute__((aligned(16)));
static uint32_t vfpu_bk_snap[3];

void vfpu_refresh_bk_trans(R3000CPU *cpu)
{
    vfpu_bk_trans[0] = (float)(int32_t)C(vc_RBK);
    vfpu_bk_trans[1] = (float)(int32_t)C(vc_GBK);
    vfpu_bk_trans[2] = (float)(int32_t)C(vc_BBK);
    vfpu_bk_trans[3] = 0.0f;

    for (int i = 0; i < 3; i++)
        vfpu_bk_snap[i] = C(vc_RBK + i);
}

int vfpu_bk_is_dirty(R3000CPU *cpu)
{
    for (int i = 0; i < 3; i++)
        if (vfpu_bk_snap[i] != C(vc_RBK + i))
            return 1;
    return 0;
}

/* Zero translation for cv=3 (None) */
float vfpu_zero_trans[4] __attribute__((aligned(16))) = {0.0f, 0.0f, 0.0f, 0.0f};

/* ================================================================
 * Get vector element (same as gte.c)
 * ================================================================ */
static int16_t get_vector_vfpu(R3000CPU *cpu, int v, int comp)
{
    switch (v) {
    case 0:
        if (comp == 0) return lo16v(D(vd_VXY0));
        if (comp == 1) return hi16v(D(vd_VXY0));
        return (int16_t)(int32_t)D(vd_VZ0);
    case 1:
        if (comp == 0) return lo16v(D(vd_VXY1));
        if (comp == 1) return hi16v(D(vd_VXY1));
        return (int16_t)(int32_t)D(vd_VZ1);
    case 2:
        if (comp == 0) return lo16v(D(vd_VXY2));
        if (comp == 1) return hi16v(D(vd_VXY2));
        return (int16_t)(int32_t)D(vd_VZ2);
    case 3:
        if (comp == 0) return (int16_t)(int32_t)D(vd_IR1);
        if (comp == 1) return (int16_t)(int32_t)D(vd_IR2);
        return (int16_t)(int32_t)D(vd_IR3);
    default:
        return 0;
    }
}

/* ================================================================
 * VFPU MVMVA: Matrix x Vector + Translation (sf=1 only)
 *
 * Same constraints as VU0 path:
 *   - mx=3 (garbage matrix) not supported -> caller falls back to C
 *   - cv=2 (FC/bugged) not supported -> caller falls back to C
 *   - sf=0 not supported -> caller falls back to C
 * ================================================================ */
uint32_t gte_flag_vfpu = 0;

void gte_mvmva_vfpu(R3000CPU *cpu, int lm, int mx, int v, int cv)
{
    /* Integer MADD path (POPS-style): exact 44-bit math using mult/madd chains.
     * float32 VFPU vtfm3.t loses precision for large values (24-bit mantissa).
     * Games rarely care, but to pass the 1150 exact-match test suite we need
     * integer arithmetic.  The VFPU matrix infrastructure is kept for potential
     * future optimizations (batch projection, etc.). */

    /* Get matrix elements */
    int16_t m[3][3];
    int base;
    switch (mx) {
    case 0: base = 0; break;     /* RT: ctrl[0..4] */
    case 1: base = 8; break;     /* LT: ctrl[8..12] */
    default: base = 16; break;   /* LC: ctrl[16..20] */
    }
    m[0][0] = lo16v(C(base));       m[0][1] = hi16v(C(base));
    m[0][2] = lo16v(C(base + 1));   m[1][0] = hi16v(C(base + 1));
    m[1][1] = lo16v(C(base + 2));   m[1][2] = hi16v(C(base + 2));
    m[2][0] = lo16v(C(base + 3));   m[2][1] = hi16v(C(base + 3));
    m[2][2] = lo16v(C(base + 4));

    /* Get translation (shifted by <<12 as per PSX GTE hardware) */
    int64_t tx1, tx2, tx3;
    if (cv == 3) {
        tx1 = tx2 = tx3 = 0;
    } else {
        int tb;
        switch (cv) {
        case 0: tb = 5; break;   /* TR: ctrl[5..7] */
        default: tb = 13; break; /* BK: ctrl[13..15] */
        }
        tx1 = (int64_t)(int32_t)C(tb)     << 12;
        tx2 = (int64_t)(int32_t)C(tb + 1) << 12;
        tx3 = (int64_t)(int32_t)C(tb + 2) << 12;
    }

    /* Get vertex */
    int16_t vx = get_vector_vfpu(cpu, v, 0);
    int16_t vy = get_vector_vfpu(cpu, v, 1);
    int16_t vz = get_vector_vfpu(cpu, v, 2);

    /* Integer multiply-accumulate (sf=1: shift >>12 at end) */
    int64_t mac1 = tx1 + (int64_t)m[0][0] * vx + (int64_t)m[0][1] * vy + (int64_t)m[0][2] * vz;
    int64_t mac2 = tx2 + (int64_t)m[1][0] * vx + (int64_t)m[1][1] * vy + (int64_t)m[1][2] * vz;
    int64_t mac3 = tx3 + (int64_t)m[2][0] * vx + (int64_t)m[2][1] * vy + (int64_t)m[2][2] * vz;

    /* sf=1: shift >>12 (caller guarantees sf=1) */
    int32_t r1 = (int32_t)(mac1 >> 12);
    int32_t r2 = (int32_t)(mac2 >> 12);
    int32_t r3 = (int32_t)(mac3 >> 12);

    D(vd_MAC1) = (uint32_t)r1;
    D(vd_MAC2) = (uint32_t)r2;
    D(vd_MAC3) = (uint32_t)r3;
    D(vd_IR1) = (uint32_t)vfpu_saturate_ir(r1, 1, lm);
    D(vd_IR2) = (uint32_t)vfpu_saturate_ir(r2, 2, lm);
    D(vd_IR3) = (uint32_t)vfpu_saturate_ir(r3, 3, lm);
}

/* ================================================================
 * VFPU RT multiply: RT_matrix × vertex + TR → mac1/mac2/mac3
 *
 * Used by RTPS/RTPT in gte.c. The post-multiply logic (IR saturation,
 * push_sz, perspective divide, push_sxy, depth cue) stays in gte.c
 * because it needs static helpers (push_sz, push_sxy, gte_divide, etc.)
 * ================================================================ */
void vfpu_rt_multiply(R3000CPU *cpu, int v, int32_t *out_mac1, int32_t *out_mac2, int32_t *out_mac3)
{
    /* Integer MADD: RT matrix × vertex + TR  (sf=1 → >>12) */
    int16_t r11 = lo16v(C(vc_RT11RT12)), r12 = hi16v(C(vc_RT11RT12));
    int16_t r13 = lo16v(C(vc_RT13RT21)), r21 = hi16v(C(vc_RT13RT21));
    int16_t r22 = lo16v(C(vc_RT22RT23)), r23 = hi16v(C(vc_RT22RT23));
    int16_t r31 = lo16v(C(vc_RT31RT32)), r32 = hi16v(C(vc_RT31RT32));
    int16_t r33 = lo16v(C(vc_RT33));

    int64_t tx = (int64_t)(int32_t)C(vc_TRX) << 12;
    int64_t ty = (int64_t)(int32_t)C(vc_TRY) << 12;
    int64_t tz = (int64_t)(int32_t)C(vc_TRZ) << 12;

    int16_t vx = get_vector_vfpu(cpu, v, 0);
    int16_t vy = get_vector_vfpu(cpu, v, 1);
    int16_t vz = get_vector_vfpu(cpu, v, 2);

    *out_mac1 = (int32_t)((tx + (int64_t)r11 * vx + (int64_t)r12 * vy + (int64_t)r13 * vz) >> 12);
    *out_mac2 = (int32_t)((ty + (int64_t)r21 * vx + (int64_t)r22 * vy + (int64_t)r23 * vz) >> 12);
    *out_mac3 = (int32_t)((tz + (int64_t)r31 * vx + (int64_t)r32 * vy + (int64_t)r33 * vz) >> 12);
}
