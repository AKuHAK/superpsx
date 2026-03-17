/**
 * gte_playground_main.c — GTE C vs VFPU comparison playground
 *
 * Standalone PSP .elf that runs GTE operations through both the exact C
 * path and the VFPU-accelerated path, comparing results with tolerance.
 *
 * Test categories:
 *   1. MVMVA with RT matrix + TR translation (typical RTPS input)
 *   2. MVMVA with Light matrix + no translation (NCS step 1)
 *   3. MVMVA with Color matrix + BK translation (NCS step 2)
 *   4. Full GTE commands: RTPS, RTPT, NCS, NCT, NCDS
 *   5. Edge cases: large values, zero matrix, identity
 *   6. Random fuzz (seed-based reproducible)
 *
 * Each test: set GTE registers → run C path (copy1) → run VFPU path
 * (copy2) → compare MAC1/2/3, IR1/2/3 with tolerance.
 */
#ifdef __PSP__
#include <pspkernel.h>
#include <pspdebug.h>

PSP_MODULE_INFO("GTE_Playground", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
#endif

#include "superpsx.h"
#include "config.h"
#include "test_all_data.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- Access to GTE internals ---- */
extern void GTE_Execute(uint32_t opcode, R3000CPU *cpu);
extern void GTE_WriteData(R3000CPU *cpu, int reg, uint32_t val);
extern void GTE_WriteCtrl(R3000CPU *cpu, int reg, uint32_t val);
extern uint32_t GTE_ReadData(R3000CPU *cpu, int reg);
extern uint32_t GTE_ReadCtrl(R3000CPU *cpu, int reg);

/* VFPU direct call */
extern void gte_mvmva_vfpu(R3000CPU *cpu, int lm, int mx, int v, int cv);
extern uint32_t gte_flag_vfpu;

/* For toggling VFPU path */
extern int gte_use_vfpu;

/* ---- Encode GTE opcodes ---- */
#define GTE_OP(func, sf, lm, mx, v, cv) \
    (0x4A000000u | (func) | ((sf) << 19) | ((lm) << 10) | ((mx) << 17) | ((v) << 15) | ((cv) << 13))

#define OP_MVMVA(sf, lm, mx, v, cv) GTE_OP(0x12, sf, lm, mx, v, cv)
#define OP_RTPS(sf, lm)             GTE_OP(0x01, sf, lm, 0, 0, 0)
#define OP_RTPT(sf, lm)             GTE_OP(0x30, sf, lm, 0, 0, 0)
#define OP_NCS(sf, lm)              GTE_OP(0x1E, sf, lm, 0, 0, 0)
#define OP_NCT(sf, lm)              GTE_OP(0x20, sf, lm, 0, 0, 0)
#define OP_NCDS(sf, lm)             GTE_OP(0x13, sf, lm, 0, 0, 0)
#define OP_NCDT(sf, lm)             GTE_OP(0x16, sf, lm, 0, 0, 0)
#define OP_NCCS(sf, lm)             GTE_OP(0x1B, sf, lm, 0, 0, 0)

/* ---- Test framework ---- */
static int total_tests = 0;
static int total_pass = 0;
static int total_fail = 0;
static int total_tolerated = 0; /* passed with tolerance (not exact match) */
static int max_delta_seen = 0;

#define TOLERANCE 1  /* allow ±1 difference in MAC values */

typedef struct {
    int32_t mac1, mac2, mac3;
    int32_t ir1, ir2, ir3;
    uint32_t flag;
} GTEResult;

static void extract_result(R3000CPU *cpu, GTEResult *r)
{
    r->mac1 = (int32_t)cpu->cp2_data[25];
    r->mac2 = (int32_t)cpu->cp2_data[26];
    r->mac3 = (int32_t)cpu->cp2_data[27];
    r->ir1  = (int32_t)cpu->cp2_data[9];
    r->ir2  = (int32_t)cpu->cp2_data[10];
    r->ir3  = (int32_t)cpu->cp2_data[11];
    r->flag = cpu->cp2_ctrl[31];
}

static int abs_val(int x) { return x < 0 ? -x : x; }

static int compare_results(const char *name, const GTEResult *c, const GTEResult *v, int tol)
{
    int d1 = abs_val(c->mac1 - v->mac1);
    int d2 = abs_val(c->mac2 - v->mac2);
    int d3 = abs_val(c->mac3 - v->mac3);
    int di1 = abs_val(c->ir1 - v->ir1);
    int di2 = abs_val(c->ir2 - v->ir2);
    int di3 = abs_val(c->ir3 - v->ir3);
    int max_d = d1;
    if (d2 > max_d) max_d = d2;
    if (d3 > max_d) max_d = d3;
    if (di1 > max_d) max_d = di1;
    if (di2 > max_d) max_d = di2;
    if (di3 > max_d) max_d = di3;

    if (max_d > max_delta_seen) max_delta_seen = max_d;
    total_tests++;

    if (max_d == 0) {
        total_pass++;
        return 1; /* exact match */
    } else if (max_d <= tol) {
        total_pass++;
        total_tolerated++;
        return 1; /* tolerated */
    } else {
        total_fail++;
        printf("FAIL %s: C=(%ld,%ld,%ld) VFPU=(%ld,%ld,%ld) delta=(%d,%d,%d) max=%d\n",
               name, (long)c->mac1, (long)c->mac2, (long)c->mac3,
               (long)v->mac1, (long)v->mac2, (long)v->mac3,
               d1, d2, d3, max_d);
        return 0;
    }
}

/* ---- Setup helpers ---- */
static void set_rt_matrix(R3000CPU *cpu, int16_t r11, int16_t r12, int16_t r13,
                          int16_t r21, int16_t r22, int16_t r23,
                          int16_t r31, int16_t r32, int16_t r33)
{
    GTE_WriteCtrl(cpu, 0, ((uint32_t)(uint16_t)r12 << 16) | (uint16_t)r11);
    GTE_WriteCtrl(cpu, 1, ((uint32_t)(uint16_t)r21 << 16) | (uint16_t)r13);
    GTE_WriteCtrl(cpu, 2, ((uint32_t)(uint16_t)r23 << 16) | (uint16_t)r22);
    GTE_WriteCtrl(cpu, 3, ((uint32_t)(uint16_t)r32 << 16) | (uint16_t)r31);
    GTE_WriteCtrl(cpu, 4, (uint16_t)r33);
}

static void set_translation(R3000CPU *cpu, int32_t tx, int32_t ty, int32_t tz)
{
    GTE_WriteCtrl(cpu, 5, (uint32_t)tx);
    GTE_WriteCtrl(cpu, 6, (uint32_t)ty);
    GTE_WriteCtrl(cpu, 7, (uint32_t)tz);
}

static void set_light_matrix(R3000CPU *cpu, int16_t l11, int16_t l12, int16_t l13,
                             int16_t l21, int16_t l22, int16_t l23,
                             int16_t l31, int16_t l32, int16_t l33)
{
    GTE_WriteCtrl(cpu, 8,  ((uint32_t)(uint16_t)l12 << 16) | (uint16_t)l11);
    GTE_WriteCtrl(cpu, 9,  ((uint32_t)(uint16_t)l21 << 16) | (uint16_t)l13);
    GTE_WriteCtrl(cpu, 10, ((uint32_t)(uint16_t)l23 << 16) | (uint16_t)l22);
    GTE_WriteCtrl(cpu, 11, ((uint32_t)(uint16_t)l32 << 16) | (uint16_t)l31);
    GTE_WriteCtrl(cpu, 12, (uint16_t)l33);
}

static void set_color_matrix(R3000CPU *cpu, int16_t r1, int16_t r2, int16_t r3,
                             int16_t g1, int16_t g2, int16_t g3,
                             int16_t b1, int16_t b2, int16_t b3)
{
    GTE_WriteCtrl(cpu, 16, ((uint32_t)(uint16_t)r2 << 16) | (uint16_t)r1);
    GTE_WriteCtrl(cpu, 17, ((uint32_t)(uint16_t)g1 << 16) | (uint16_t)r3);
    GTE_WriteCtrl(cpu, 18, ((uint32_t)(uint16_t)g3 << 16) | (uint16_t)g2);
    GTE_WriteCtrl(cpu, 19, ((uint32_t)(uint16_t)b2 << 16) | (uint16_t)b1);
    GTE_WriteCtrl(cpu, 20, (uint16_t)b3);
}

static void set_bk_color(R3000CPU *cpu, int32_t rbk, int32_t gbk, int32_t bbk)
{
    GTE_WriteCtrl(cpu, 13, (uint32_t)rbk);
    GTE_WriteCtrl(cpu, 14, (uint32_t)gbk);
    GTE_WriteCtrl(cpu, 15, (uint32_t)bbk);
}

static void set_vertex0(R3000CPU *cpu, int16_t vx, int16_t vy, int16_t vz)
{
    GTE_WriteData(cpu, 0, ((uint32_t)(uint16_t)vy << 16) | (uint16_t)vx);
    GTE_WriteData(cpu, 1, (uint32_t)(uint16_t)vz);
}

static void set_vertex1(R3000CPU *cpu, int16_t vx, int16_t vy, int16_t vz)
{
    GTE_WriteData(cpu, 2, ((uint32_t)(uint16_t)vy << 16) | (uint16_t)vx);
    GTE_WriteData(cpu, 3, (uint32_t)(uint16_t)vz);
}

static void set_vertex2(R3000CPU *cpu, int16_t vx, int16_t vy, int16_t vz)
{
    GTE_WriteData(cpu, 4, ((uint32_t)(uint16_t)vy << 16) | (uint16_t)vx);
    GTE_WriteData(cpu, 5, (uint32_t)(uint16_t)vz);
}

static void set_h_ofx_ofy(R3000CPU *cpu, uint16_t h, int32_t ofx, int32_t ofy)
{
    GTE_WriteCtrl(cpu, 26, h);
    GTE_WriteCtrl(cpu, 24, (uint32_t)ofx);
    GTE_WriteCtrl(cpu, 25, (uint32_t)ofy);
}

static void set_dqa_dqb(R3000CPU *cpu, int16_t dqa, int32_t dqb)
{
    GTE_WriteCtrl(cpu, 27, (uint32_t)(uint16_t)dqa);
    GTE_WriteCtrl(cpu, 28, (uint32_t)dqb);
}

static void set_zsf(R3000CPU *cpu, int16_t zsf3, int16_t zsf4)
{
    GTE_WriteCtrl(cpu, 29, (uint32_t)(uint16_t)zsf3);
    GTE_WriteCtrl(cpu, 30, (uint32_t)(uint16_t)zsf4);
}

/* ---- Simple PRNG (deterministic) ---- */
static uint32_t rng_state = 0xDEADBEEF;
static uint32_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static int16_t rng_i16(void) { return (int16_t)(rng_next() & 0xFFFF); }
static int16_t rng_i16_small(void) { return (int16_t)((rng_next() % 4001) - 2000); }
static int32_t rng_i32_small(void) { return (int32_t)((rng_next() % 200001) - 100000); }

/* ================================================================
 * Test: MVMVA comparison (C vs VFPU)
 *
 * For each test case:
 *   1. Setup GTE state in cpu_c and cpu_v (identical copies)
 *   2. Run C path on cpu_c (gte_use_vfpu=0)
 *   3. Run VFPU path on cpu_v (gte_use_vfpu=1)
 *   4. Compare MAC1/2/3, IR1/2/3
 * ================================================================ */
static R3000CPU cpu_c, cpu_v;

static void run_compare_op(const char *name, uint32_t opcode, int tol)
{
    /* Run C path */
    gte_use_vfpu = 0;
    GTE_Execute(opcode, &cpu_c);
    GTEResult rc;
    extract_result(&cpu_c, &rc);

    /* Run VFPU path */
    gte_use_vfpu = 1;
    GTE_Execute(opcode, &cpu_v);
    GTEResult rv;
    extract_result(&cpu_v, &rv);

    compare_results(name, &rc, &rv, tol);
}

static void setup_typical_rt(R3000CPU *cpu)
{
    /* Crash Bandicoot-like rotation matrix (near identity, scaled by 4096) */
    set_rt_matrix(cpu, 4096, 0, 0, 0, 4096, 0, 0, 0, 4096);
    set_translation(cpu, 100, 200, 1000);
}

static void setup_typical_light(R3000CPU *cpu)
{
    set_light_matrix(cpu, 2048, 1024, 512, -512, 2048, -1024, 1024, -512, 2048);
}

static void setup_typical_color(R3000CPU *cpu)
{
    set_color_matrix(cpu, 3200, 0, 0, 0, 3200, 0, 0, 0, 3200);
    set_bk_color(cpu, 50, 50, 50);
}

/* ================================================================
 * Test categories
 * ================================================================ */

static void test_mvmva_rt_basic(void)
{
    printf("=== MVMVA RT basic ===\n");

    /* Test 1: Identity-like matrix, small vertex */
    memset(&cpu_c, 0, sizeof(cpu_c));
    setup_typical_rt(&cpu_c);
    set_vertex0(&cpu_c, 100, -50, 200);
    memcpy(&cpu_v, &cpu_c, sizeof(R3000CPU));
    run_compare_op("RT identity small", OP_MVMVA(1, 1, 0, 0, 0), TOLERANCE);

    /* Test 2: Rotated matrix */
    memset(&cpu_c, 0, sizeof(cpu_c));
    set_rt_matrix(&cpu_c, 3547, 0, -2048, 0, 4096, 0, 2048, 0, 3547);
    set_translation(&cpu_c, 500, -300, 2000);
    set_vertex0(&cpu_c, 1000, -500, 300);
    memcpy(&cpu_v, &cpu_c, sizeof(R3000CPU));
    run_compare_op("RT rotated", OP_MVMVA(1, 1, 0, 0, 0), TOLERANCE);

    /* Test 3: Zero vertex */
    memset(&cpu_c, 0, sizeof(cpu_c));
    setup_typical_rt(&cpu_c);
    set_vertex0(&cpu_c, 0, 0, 0);
    memcpy(&cpu_v, &cpu_c, sizeof(R3000CPU));
    run_compare_op("RT zero vertex", OP_MVMVA(1, 1, 0, 0, 0), TOLERANCE);

    /* Test 4: Large vertex values */
    memset(&cpu_c, 0, sizeof(cpu_c));
    setup_typical_rt(&cpu_c);
    set_vertex0(&cpu_c, 32000, -32000, 16000);
    memcpy(&cpu_v, &cpu_c, sizeof(R3000CPU));
    run_compare_op("RT large vertex", OP_MVMVA(1, 1, 0, 0, 0), TOLERANCE);

    /* Test 5: cv=3 (no translation) */
    memset(&cpu_c, 0, sizeof(cpu_c));
    setup_typical_rt(&cpu_c);
    set_vertex0(&cpu_c, 100, 200, 300);
    memcpy(&cpu_v, &cpu_c, sizeof(R3000CPU));
    run_compare_op("RT no trans", OP_MVMVA(1, 1, 0, 0, 3), TOLERANCE);
}

static void test_mvmva_light(void)
{
    printf("=== MVMVA Light matrix ===\n");

    memset(&cpu_c, 0, sizeof(cpu_c));
    setup_typical_light(&cpu_c);
    set_vertex0(&cpu_c, 500, -200, 700);
    memcpy(&cpu_v, &cpu_c, sizeof(R3000CPU));
    run_compare_op("LT * V0, no trans", OP_MVMVA(1, 1, 1, 0, 3), TOLERANCE);

    /* With IR as vector (v=3) */
    memset(&cpu_c, 0, sizeof(cpu_c));
    setup_typical_light(&cpu_c);
    GTE_WriteData(&cpu_c, 9,  (uint32_t)(int32_t)1000);
    GTE_WriteData(&cpu_c, 10, (uint32_t)(int32_t)-500);
    GTE_WriteData(&cpu_c, 11, (uint32_t)(int32_t)2000);
    memcpy(&cpu_v, &cpu_c, sizeof(R3000CPU));
    run_compare_op("LT * IR, no trans", OP_MVMVA(1, 1, 1, 3, 3), TOLERANCE);
}

static void test_mvmva_color(void)
{
    printf("=== MVMVA Color matrix ===\n");

    memset(&cpu_c, 0, sizeof(cpu_c));
    setup_typical_color(&cpu_c);
    GTE_WriteData(&cpu_c, 9,  (uint32_t)(int32_t)1000);
    GTE_WriteData(&cpu_c, 10, (uint32_t)(int32_t)500);
    GTE_WriteData(&cpu_c, 11, (uint32_t)(int32_t)800);
    memcpy(&cpu_v, &cpu_c, sizeof(R3000CPU));
    run_compare_op("LCM * IR + BK", OP_MVMVA(1, 1, 2, 3, 1), TOLERANCE);
}

static void test_full_rtps(void)
{
    printf("=== Full RTPS/RTPT ===\n");

    memset(&cpu_c, 0, sizeof(cpu_c));
    setup_typical_rt(&cpu_c);
    set_vertex0(&cpu_c, 100, -50, 200);
    set_h_ofx_ofy(&cpu_c, 0x155, 0x01000000, 0x00F80000);
    set_dqa_dqb(&cpu_c, (int16_t)0xFE00, 0x01400000);
    set_zsf(&cpu_c, 0x155, 0x100);
    memcpy(&cpu_v, &cpu_c, sizeof(R3000CPU));
    run_compare_op("RTPS basic", OP_RTPS(1, 1), TOLERANCE);

    /* RTPT: 3 vertices */
    memset(&cpu_c, 0, sizeof(cpu_c));
    setup_typical_rt(&cpu_c);
    set_vertex0(&cpu_c, 100, -50, 200);
    set_vertex1(&cpu_c, -200, 100, 300);
    set_vertex2(&cpu_c, 50, 50, 500);
    set_h_ofx_ofy(&cpu_c, 0x155, 0x01000000, 0x00F80000);
    set_dqa_dqb(&cpu_c, (int16_t)0xFE00, 0x01400000);
    set_zsf(&cpu_c, 0x155, 0x100);
    memcpy(&cpu_v, &cpu_c, sizeof(R3000CPU));
    run_compare_op("RTPT 3 verts", OP_RTPT(1, 1), TOLERANCE);
}

static void test_full_ncs(void)
{
    printf("=== Full NCS/NCT/NCDS ===\n");

    memset(&cpu_c, 0, sizeof(cpu_c));
    setup_typical_light(&cpu_c);
    setup_typical_color(&cpu_c);
    set_vertex0(&cpu_c, 0, 4096, 0);
    GTE_WriteData(&cpu_c, 6, 0x00808080); /* RGBC = white */
    /* FC for depth cue */
    GTE_WriteCtrl(&cpu_c, 21, 0x100); /* RFC */
    GTE_WriteCtrl(&cpu_c, 22, 0x100); /* GFC */
    GTE_WriteCtrl(&cpu_c, 23, 0x100); /* BFC */
    memcpy(&cpu_v, &cpu_c, sizeof(R3000CPU));
    run_compare_op("NCS basic", OP_NCS(1, 1), TOLERANCE);

    /* NCDS: normal + depth cue */
    memset(&cpu_c, 0, sizeof(cpu_c));
    setup_typical_light(&cpu_c);
    setup_typical_color(&cpu_c);
    set_vertex0(&cpu_c, 2048, 2048, 2048);
    GTE_WriteData(&cpu_c, 6, 0x00C0C0C0); /* RGBC */
    GTE_WriteData(&cpu_c, 8, (uint32_t)(int32_t)4000); /* IR0 */
    GTE_WriteCtrl(&cpu_c, 21, 0x100);
    GTE_WriteCtrl(&cpu_c, 22, 0x100);
    GTE_WriteCtrl(&cpu_c, 23, 0x100);
    memcpy(&cpu_v, &cpu_c, sizeof(R3000CPU));
    run_compare_op("NCDS basic", OP_NCDS(1, 1), TOLERANCE);

    /* NCCS */
    memset(&cpu_c, 0, sizeof(cpu_c));
    setup_typical_light(&cpu_c);
    setup_typical_color(&cpu_c);
    set_vertex0(&cpu_c, 0, 4096, 0);
    GTE_WriteData(&cpu_c, 6, 0x00808080);
    memcpy(&cpu_v, &cpu_c, sizeof(R3000CPU));
    run_compare_op("NCCS basic", OP_NCCS(1, 1), TOLERANCE);
}

static void test_fuzz_mvmva(void)
{
    printf("=== Fuzz MVMVA (500 random) ===\n");
    char name[64];
    rng_state = 0xC0FFEE42;

    for (int i = 0; i < 500; i++) {
        memset(&cpu_c, 0, sizeof(cpu_c));

        /* Random matrix with typical-ish values */
        set_rt_matrix(&cpu_c,
            rng_i16_small(), rng_i16_small(), rng_i16_small(),
            rng_i16_small(), rng_i16_small(), rng_i16_small(),
            rng_i16_small(), rng_i16_small(), rng_i16_small());
        set_translation(&cpu_c, rng_i32_small(), rng_i32_small(), rng_i32_small());
        set_vertex0(&cpu_c, rng_i16_small(), rng_i16_small(), rng_i16_small());

        memcpy(&cpu_v, &cpu_c, sizeof(R3000CPU));
        snprintf(name, sizeof(name), "fuzz_mvmva_%d", i);
        run_compare_op(name, OP_MVMVA(1, 1, 0, 0, 0), TOLERANCE);
    }
}

static void test_fuzz_mvmva_extreme(void)
{
    printf("=== Fuzz MVMVA extreme (200 random, full range) ===\n");
    char name[64];
    rng_state = 0xBADC0DE;

    for (int i = 0; i < 200; i++) {
        memset(&cpu_c, 0, sizeof(cpu_c));

        /* Full-range int16 matrix and vector */
        set_rt_matrix(&cpu_c,
            rng_i16(), rng_i16(), rng_i16(),
            rng_i16(), rng_i16(), rng_i16(),
            rng_i16(), rng_i16(), rng_i16());
        set_translation(&cpu_c,
            (int32_t)rng_next(), (int32_t)rng_next(), (int32_t)rng_next());
        set_vertex0(&cpu_c, rng_i16(), rng_i16(), rng_i16());

        memcpy(&cpu_v, &cpu_c, sizeof(R3000CPU));
        snprintf(name, sizeof(name), "fuzz_extreme_%d", i);

        /* Use larger tolerance for extreme values:
         * 32768 * 32768 / 4096 = 262144 → 18 bits, well within float32.
         * But sum of 3 can reach 786432 → 20 bits. Translation adds up to
         * 2^31. float32 can't represent ±2^31 exactly (24-bit mantissa).
         * For large translations, error can be up to 2^(31-24) = 128. */
        run_compare_op(name, OP_MVMVA(1, 1, 0, 0, 0), 256);
    }
}

static void test_fuzz_full_cmds(void)
{
    printf("=== Fuzz full GTE commands (100 random) ===\n");
    char name[64];
    rng_state = 0x12345678;

    uint32_t ops[] = {
        OP_RTPS(1, 1), OP_RTPT(1, 1), OP_NCS(1, 1),
        OP_NCT(1, 1), OP_NCDS(1, 1), OP_NCDT(1, 1), OP_NCCS(1, 1),
    };
    const char *op_names[] = {
        "RTPS", "RTPT", "NCS", "NCT", "NCDS", "NCDT", "NCCS",
    };
    int n_ops = sizeof(ops) / sizeof(ops[0]);

    for (int i = 0; i < 100; i++) {
        memset(&cpu_c, 0, sizeof(cpu_c));

        /* Set up all matrices with small random values */
        set_rt_matrix(&cpu_c,
            rng_i16_small(), rng_i16_small(), rng_i16_small(),
            rng_i16_small(), rng_i16_small(), rng_i16_small(),
            rng_i16_small(), rng_i16_small(), rng_i16_small());
        set_translation(&cpu_c, rng_i32_small(), rng_i32_small(), rng_i32_small());
        set_light_matrix(&cpu_c,
            rng_i16_small(), rng_i16_small(), rng_i16_small(),
            rng_i16_small(), rng_i16_small(), rng_i16_small(),
            rng_i16_small(), rng_i16_small(), rng_i16_small());
        setup_typical_color(&cpu_c);
        set_bk_color(&cpu_c, rng_i32_small() / 100, rng_i32_small() / 100, rng_i32_small() / 100);

        set_vertex0(&cpu_c, rng_i16_small(), rng_i16_small(), rng_i16_small());
        set_vertex1(&cpu_c, rng_i16_small(), rng_i16_small(), rng_i16_small());
        set_vertex2(&cpu_c, rng_i16_small(), rng_i16_small(), rng_i16_small());
        GTE_WriteData(&cpu_c, 6, 0x00808080); /* RGBC */
        GTE_WriteData(&cpu_c, 8, (uint32_t)(int32_t)(256 + (rng_next() % 3840)));  /* IR0 */
        GTE_WriteCtrl(&cpu_c, 21, 0x100); /* RFC */
        GTE_WriteCtrl(&cpu_c, 22, 0x100); /* GFC */
        GTE_WriteCtrl(&cpu_c, 23, 0x100); /* BFC */
        set_h_ofx_ofy(&cpu_c, 0x155, 0x01000000, 0x00F80000);
        set_dqa_dqb(&cpu_c, (int16_t)0xFE00, 0x01400000);
        set_zsf(&cpu_c, 0x155, 0x100);

        int op_idx = rng_next() % n_ops;
        memcpy(&cpu_v, &cpu_c, sizeof(R3000CPU));
        snprintf(name, sizeof(name), "fuzz_%s_%d", op_names[op_idx], i);
        run_compare_op(name, ops[op_idx], TOLERANCE);
    }
}

/* ================================================================
 * Test: all 1150 test cases from ps1-tests/gte/test-all
 *
 * For each test: load input regs into cpu_c and cpu_v,
 * run C path on cpu_c, VFPU/fast path on cpu_v, compare
 * all 64 output registers with tolerance.
 * ================================================================ */

/* Write all 64 GTE registers (0-31=data, 32-63=ctrl) */
static void load_all_regs(R3000CPU *cpu, const uint32_t input[64])
{
    for (int i = 0; i < 32; i++)
        GTE_WriteData(cpu, i, input[i]);
    for (int i = 0; i < 32; i++)
        GTE_WriteCtrl(cpu, i, input[32 + i]);
}

/* Read all 64 GTE registers */
static void read_all_regs(R3000CPU *cpu, uint32_t output[64])
{
    for (int i = 0; i < 32; i++)
        output[i] = GTE_ReadData(cpu, i);
    for (int i = 0; i < 32; i++)
        output[32 + i] = GTE_ReadCtrl(cpu, i);
}

/* GTE register names for diagnostics */
static const char *gte_reg_name(int r)
{
    static const char *data_names[] = {
        "VXY0","VZ0","VXY1","VZ1","VXY2","VZ2","RGBC","OTZ",
        "IR0","IR1","IR2","IR3","SXY0","SXY1","SXY2","SXYP",
        "SZ0","SZ1","SZ2","SZ3","RGB0","RGB1","RGB2","RES1",
        "MAC0","MAC1","MAC2","MAC3","IRGB","ORGB","LZCS","LZCR"
    };
    static const char *ctrl_names[] = {
        "RT11RT12","RT13RT21","RT22RT23","RT31RT32","RT33",
        "TRX","TRY","TRZ","L11L12","L13L21","L22L23","L31L32","L33",
        "RBK","GBK","BBK","LR1LR2","LR3LG1","LG2LG3","LB1LB2","LB3",
        "RFC","GFC","BFC","OFX","OFY","H","DQA","DQB",
        "ZSF3","ZSF4","FLAG"
    };
    static char buf[16];
    if (r < 32)
        return data_names[r];
    else if (r - 32 < 31)
        return ctrl_names[r - 32];
    snprintf(buf, sizeof(buf), "r%d", r);
    return buf;
}

/* Per-register tolerance: most regs ±1, FLAG ignored, MAC0 ±2 */
static int reg_tolerance(int r)
{
    if (r == 63) return -1;   /* FLAG (ctrl 31): skip comparison */
    if (r == 24) return 2;    /* MAC0: FPU rounding */
    if (r == 14) return 2;    /* SXY1: projection rounding */
    if (r == 15) return 2;    /* SXY2/SXYP: projection rounding */
    return TOLERANCE;         /* default ±1 */
}

static void test_all_comparison(void)
{
    printf("=== test-all: 1150 tests (C vs VFPU) ===\n");

    int ta_pass = 0, ta_fail = 0, ta_tolerated = 0;
    int ta_c_exact = 0;  /* C path matches expected output exactly */
    int ta_max_delta = 0;

    for (int t = 0; t < TEST_COUNT; t++) {
        const struct test_t *tc = &tests[t];

        /* Load identical state into both CPUs */
        memset(&cpu_c, 0, sizeof(cpu_c));
        memset(&cpu_v, 0, sizeof(cpu_v));
        load_all_regs(&cpu_c, tc->input);
        load_all_regs(&cpu_v, tc->input);

        /* Execute op (if any) */
        if (tc->opcode != 0xffffffff) {
            gte_use_vfpu = 0;
            GTE_Execute(tc->opcode, &cpu_c);

            gte_use_vfpu = 1;
            GTE_Execute(tc->opcode, &cpu_v);
        }

        /* Read back all registers */
        uint32_t out_c[64], out_v[64];
        read_all_regs(&cpu_c, out_c);
        read_all_regs(&cpu_v, out_v);

        /* Sanity: check C path vs expected output */
        int c_exact = 1;
        for (int r = 0; r < 64; r++) {
            if (out_c[r] != tc->output[r]) { c_exact = 0; break; }
        }
        if (c_exact) ta_c_exact++;

        /* Compare C vs VFPU */
        int test_max = 0;
        int test_fail = 0;
        int worst_reg = -1;
        for (int r = 0; r < 64; r++) {
            int tol = reg_tolerance(r);
            if (tol < 0) continue; /* skip this register */

            int32_t diff = (int32_t)(out_v[r] - out_c[r]);
            int adiff = diff < 0 ? -diff : diff;
            if (adiff > test_max) { test_max = adiff; worst_reg = r; }
            if (adiff > tol) test_fail = 1;
        }

        if (test_fail) {
            ta_fail++;
            /* One-line summary for each failing test */
            printf("  FAIL %s (op=0x%08lx) worst=r%d(%s) delta=%d C=0x%08lx V=0x%08lx\n",
                   tc->name, (unsigned long)tc->opcode,
                   worst_reg, gte_reg_name(worst_reg),
                   test_max,
                   (unsigned long)out_c[worst_reg],
                   (unsigned long)out_v[worst_reg]);
        } else if (test_max > 0) {
            ta_pass++;
            ta_tolerated++;
        } else {
            ta_pass++;
        }
        if (test_max > ta_max_delta) ta_max_delta = test_max;
    }

    total_tests += ta_pass + ta_fail;
    total_pass += ta_pass;
    total_fail += ta_fail;
    total_tolerated += ta_tolerated;
    if (ta_max_delta > max_delta_seen) max_delta_seen = ta_max_delta;

    printf("  C matches expected: %d/%d\n", ta_c_exact, TEST_COUNT);
    printf("  C vs VFPU: %d/%d passed", ta_pass, ta_pass + ta_fail);
    if (ta_fail > 0) printf(", %d FAILED", ta_fail);
    if (ta_tolerated > 0) printf(", %d tolerated", ta_tolerated);
    printf(", max_delta=%d\n", ta_max_delta);
}

/* ================================================================
 * Main entry point
 * ================================================================ */

/* Stub: config needs to exist */
PSXConfig psx_config;

/* Stub: host_log used by gte.c */
void host_log_printf(const char *fmt, ...) { (void)fmt; }
void host_log_flush(void) {}

int main(void)
{
#ifdef __PSP__
    pspDebugScreenInit();
    pspDebugScreenPrintf("GTE Playground: C vs VFPU comparison\n\n");
#endif

    printf("GTE Playground: C vs VFPU comparison\n");
    printf("Tolerance: +/-%d per MAC/IR value\n\n", TOLERANCE);

    /* Force gte_use_vu0=0 so the base GTE always uses C */
    psx_config.gte_vu0 = 0;

    test_mvmva_rt_basic();
    test_mvmva_light();
    test_mvmva_color();
    test_full_rtps();
    test_full_ncs();
    test_fuzz_mvmva();
    test_fuzz_mvmva_extreme();
    test_fuzz_full_cmds();
    test_all_comparison();

    printf("\n========================================\n");
    printf("Results: %d/%d passed", total_pass, total_tests);
    if (total_fail > 0) printf(", %d FAILED", total_fail);
    if (total_tolerated > 0) printf(", %d tolerated", total_tolerated);
    printf("\nMax delta seen: %d\n", max_delta_seen);
    printf("========================================\n");

#ifdef __PSP__
    pspDebugScreenPrintf("\nResults: %d/%d passed", total_pass, total_tests);
    if (total_fail > 0) pspDebugScreenPrintf(", %d FAILED", total_fail);
    if (total_tolerated > 0) pspDebugScreenPrintf(", %d tolerated", total_tolerated);
    pspDebugScreenPrintf("\nMax delta: %d\n", max_delta_seen);
    sceKernelExitGame();
#endif
    return 0;
}
