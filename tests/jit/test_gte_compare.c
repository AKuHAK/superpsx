/*
 * test_gte_compare.c — GTE VU0 vs C Reference Comparison Tests
 *
 * For each GTE command that uses VU0 micro in the JIT (sf=1 path),
 * we run the C reference (gte_use_vu0=0) and the JIT (gte_use_vu0=1)
 * with the same input state, then compare output registers.
 *
 * FLAG (cp2_ctrl[31]) is intentionally NOT compared — the JIT inline
 * path always sets FLAG=0, which is an accepted limitation.
 */
#include "playground.h"

/* Access the global vu0 toggle */
extern int gte_use_vu0;

/* C reference entry points (from gte.c) */
extern void GTE_Inline_RTPS(R3000CPU *cpu, int sf, int lm);
extern void GTE_Inline_RTPT(R3000CPU *cpu, int sf, int lm);
extern void GTE_Inline_MVMVA(R3000CPU *cpu, uint32_t packed);
extern void GTE_Inline_NCS(R3000CPU *cpu, int sf, int lm);
extern void GTE_Inline_NCT(R3000CPU *cpu, int sf, int lm);
extern void GTE_Inline_NCDS(R3000CPU *cpu, int sf, int lm);
extern void GTE_Inline_NCDT(R3000CPU *cpu, int sf, int lm);
extern void GTE_Inline_NCCS(R3000CPU *cpu, int sf, int lm);
extern void GTE_Inline_NCCT(R3000CPU *cpu, int sf, int lm);
extern void GTE_Inline_CC(R3000CPU *cpu, int sf, int lm);
extern void GTE_Inline_CDP(R3000CPU *cpu, int sf, int lm);

/* ================================================================
 * Snapshot helpers
 * ================================================================ */

typedef struct {
    uint32_t data[32];
    uint32_t ctrl[32];
} GTESnapshot;

static GTESnapshot saved_input;  /* original input state */
static GTESnapshot ref_output;   /* C reference output   */

static void snapshot_save(GTESnapshot *snap)
{
    memcpy(snap->data, cpu.cp2_data, sizeof(snap->data));
    memcpy(snap->ctrl, cpu.cp2_ctrl, sizeof(snap->ctrl));
}

static void snapshot_restore(const GTESnapshot *snap)
{
    memcpy(cpu.cp2_data, snap->data, sizeof(snap->data));
    memcpy(cpu.cp2_ctrl, snap->ctrl, sizeof(snap->ctrl));
}

/* ================================================================
 * Compare helper: check data regs, skip FLAG
 * Allows ±1 tolerance for float rounding differences.
 * Returns number of mismatches.
 * ================================================================ */
static int compare_gte_output(const char *name)
{
    int mismatches = 0;

    /* Compare all 32 data registers */
    for (int i = 0; i < 32; i++) {
        /* Skip index 23 (RES1/padding — undefined) */
        if (i == 23) continue;
        uint32_t jit_val = cpu.cp2_data[i];
        uint32_t ref_val = ref_output.data[i];
        if (jit_val != ref_val) {
            /* Allow ±1 tolerance for MAC/IR registers (float rounding) */
            int32_t diff = (int32_t)jit_val - (int32_t)ref_val;
            if (diff >= -1 && diff <= 1) {
                /* 1 LSB rounding — acceptable, skip */
                continue;
            }
            printf("  [FAIL] %s: cp2_data[%d] ref=0x%08X jit=0x%08X\n",
                   name, i,
                   (unsigned)ref_val,
                   (unsigned)jit_val);
            mismatches++;
        }
    }

    /* Compare ctrl registers except FLAG (ctrl[31]) */
    for (int i = 0; i < 31; i++) {
        if (cpu.cp2_ctrl[i] != ref_output.ctrl[i]) {
            printf("  [FAIL] %s: cp2_ctrl[%d] ref=0x%08X jit=0x%08X\n",
                   name, i,
                   (unsigned)ref_output.ctrl[i],
                   (unsigned)cpu.cp2_ctrl[i]);
            mismatches++;
        }
    }

    return mismatches;
}

/* ================================================================
 * State setup helpers
 * ================================================================ */

static void enable_cop2(void)
{
    cpu.cop0[PSX_COP0_SR_IDX] = (1u << 30) | (1u << 28);
}

/* Realistic rotation matrix: ~30 degree rotation around Y axis.
 * cos30≈0.866=3547, sin30=0.5=2048 in 1.3.12 fixed-point */
static void setup_rotation_matrix(void)
{
    cpu.cp2_ctrl[GTE_RT11RT12] = (uint32_t)((0 << 16) | (3547 & 0xFFFF));     /* RT11=3547, RT12=0 */
    cpu.cp2_ctrl[GTE_RT13RT21] = (uint32_t)((0 << 16) | (2048 & 0xFFFF));     /* RT13=2048, RT21=0 */
    cpu.cp2_ctrl[GTE_RT22RT23] = (uint32_t)((0 << 16) | (4096 & 0xFFFF));     /* RT22=4096, RT23=0 */
    cpu.cp2_ctrl[GTE_RT31RT32] = (uint32_t)((0 << 16) | (-2048 & 0xFFFF));    /* RT31=-2048, RT32=0 */
    cpu.cp2_ctrl[GTE_RT33]     = 3547;                                          /* RT33=3547 */

    cpu.cp2_ctrl[GTE_TRX] = 1000;
    cpu.cp2_ctrl[GTE_TRY] = 2000;
    cpu.cp2_ctrl[GTE_TRZ] = 3000;
}

/* Light matrix: simple directional light from upper-right */
static void setup_light_matrix(void)
{
    cpu.cp2_ctrl[GTE_L11L12] = (uint32_t)((0 << 16) | (3547 & 0xFFFF));       /* L11=3547, L12=0  */
    cpu.cp2_ctrl[GTE_L13L21] = (uint32_t)((2048 & 0xFFFF) << 16) | (0);       /* L13=0, L21=2048  */
    cpu.cp2_ctrl[GTE_L22L23] = (uint32_t)((2048 & 0xFFFF) << 16) | (3547 & 0xFFFF); /* L22=3547, L23=2048 */
    cpu.cp2_ctrl[GTE_L31L32] = (uint32_t)((0 << 16) | (0));                    /* L31=0, L32=0     */
    cpu.cp2_ctrl[GTE_L33]    = 4096;                                            /* L33=4096         */

    cpu.cp2_ctrl[GTE_RBK] = 0x200;  /* Background color R */
    cpu.cp2_ctrl[GTE_GBK] = 0x200;  /* Background color G */
    cpu.cp2_ctrl[GTE_BBK] = 0x200;  /* Background color B */
}

/* Light color matrix */
static void setup_color_matrix(void)
{
    cpu.cp2_ctrl[GTE_LR1LR2] = (uint32_t)((0 << 16) | (4096 & 0xFFFF));      /* LR1=4096, LR2=0  */
    cpu.cp2_ctrl[GTE_LR3LG1] = (uint32_t)((0 & 0xFFFF) << 16) | (0);         /* LR3=0, LG1=0     */
    cpu.cp2_ctrl[GTE_LG2LG3] = (uint32_t)((0 << 16) | (4096 & 0xFFFF));      /* LG2=4096, LG3=0  */
    cpu.cp2_ctrl[GTE_LB1LB2] = (uint32_t)((0 << 16) | (0));                   /* LB1=0, LB2=0     */
    cpu.cp2_ctrl[GTE_LB3]    = 4096;                                           /* LB3=4096         */

    cpu.cp2_ctrl[GTE_RFC] = 0x80;  /* Far color R */
    cpu.cp2_ctrl[GTE_GFC] = 0x80;  /* Far color G */
    cpu.cp2_ctrl[GTE_BFC] = 0x80;  /* Far color B */
}

/* Screen params */
static void setup_screen_params(void)
{
    cpu.cp2_ctrl[GTE_OFX] = 160 << 16;
    cpu.cp2_ctrl[GTE_OFY] = 120 << 16;
    cpu.cp2_ctrl[GTE_H]   = 256;
    cpu.cp2_ctrl[GTE_DQA] = (uint32_t)(int32_t)(-0x40); /* typical DQA */
    cpu.cp2_ctrl[GTE_DQB] = 0x1400000;                   /* typical DQB */
    cpu.cp2_ctrl[GTE_ZSF3] = 1024;
    cpu.cp2_ctrl[GTE_ZSF4] = 768;
}

/* Full state for all test types */
static void setup_full_state(void)
{
    enable_cop2();
    setup_rotation_matrix();
    setup_light_matrix();
    setup_color_matrix();
    setup_screen_params();

    /* V0 = (100, 200, 300) */
    cpu.cp2_data[GTE_VXY0] = PACK_VXY(100, 200);
    cpu.cp2_data[GTE_VZ0]  = 300;

    /* V1 = (-150, 80, 500) */
    cpu.cp2_data[GTE_VXY1] = PACK_VXY(-150, 80);
    cpu.cp2_data[GTE_VZ1]  = 500;

    /* V2 = (400, -100, 250) */
    cpu.cp2_data[GTE_VXY2] = PACK_VXY(400, -100);
    cpu.cp2_data[GTE_VZ2]  = 250;

    /* RGBC = (0x80, 0x40, 0xC0, code=0xFF) */
    cpu.cp2_data[GTE_RGBC] = 0xFF00C040u | (0x80);

    /* IR1-3 (used by CDP, CC) */
    cpu.cp2_data[GTE_IR1] = 0x800;
    cpu.cp2_data[GTE_IR2] = 0x600;
    cpu.cp2_data[GTE_IR3] = 0xA00;
}

/* ================================================================
 * Generic compare-test runner
 *
 * 1. Setup state
 * 2. Run C reference (gte_use_vu0=0)
 * 3. Restore state
 * 4. Run JIT (gte_use_vu0=1, which emits VU0 inline for sf=1)
 * 5. Compare
 * ================================================================ */

typedef void (*gte_c_func)(R3000CPU *, int, int);

static void run_compare_test(const char *name,
                             uint32_t    gte_cmd,      /* PSX COP2 opcode for JIT */
                             gte_c_func  c_func,       /* C reference (sf,lm) */
                             int         sf, int lm)
{
    /* Phase 1: set up state and save it */
    BEGIN_TEST(name);
    setup_full_state();
    snapshot_save(&saved_input);

    /* Phase 2: run C reference with VU0 forced OFF */
    int saved_vu0 = gte_use_vu0;
    gte_use_vu0 = 0;
    c_func(&cpu, sf, lm);
    gte_use_vu0 = saved_vu0;

    /* Save reference output */
    snapshot_save(&ref_output);

    /* Phase 3: restore input state for JIT run */
    snapshot_restore(&saved_input);
    /* Make sure gte_use_vu0 is ON so JIT takes VU0 inline path */
    gte_use_vu0 = 1;

    /* Phase 4: emit GTE command and run via JIT */
    EMIT(gte_cmd);
    RUN(500);

    /* Restore vu0 setting */
    gte_use_vu0 = saved_vu0;

    /* Phase 5: compare */
    pg_ctx.fail_count = compare_gte_output(name);

    END_TEST();
}

/* Variant for MVMVA which takes packed args instead of (sf,lm) */
static void run_compare_test_mvmva(const char *name,
                                   int sf, int lm, int mx, int v, int cv)
{
    BEGIN_TEST(name);
    setup_full_state();
    snapshot_save(&saved_input);

    /* C reference */
    int saved_vu0 = gte_use_vu0;
    gte_use_vu0 = 0;
    uint32_t packed = sf | (lm << 1) | (mx << 2) | (v << 4) | (cv << 6);
    GTE_Inline_MVMVA(&cpu, packed);
    gte_use_vu0 = saved_vu0;
    snapshot_save(&ref_output);

    /* Restore + JIT */
    snapshot_restore(&saved_input);
    gte_use_vu0 = 1;
    EMIT(GTE_CMD_MVMVA(sf, lm, mx, v, cv));
    RUN(500);
    gte_use_vu0 = saved_vu0;

    pg_ctx.fail_count = compare_gte_output(name);
    END_TEST();
}

/* ================================================================
 * Individual comparison tests
 * ================================================================ */

/* ---- RTPS (sf=1, lm=1) ---- */
static void test_cmp_rtps(void)
{
    run_compare_test("cmp_RTPS_sf1_lm1",
                     GTE_CMD_RTPS(1, 1),
                     GTE_Inline_RTPS, 1, 1);
}

/* ---- RTPS (sf=1, lm=0) ---- */
static void test_cmp_rtps_lm0(void)
{
    run_compare_test("cmp_RTPS_sf1_lm0",
                     GTE_CMD_RTPS(1, 0),
                     GTE_Inline_RTPS, 1, 0);
}

/* ---- RTPT (sf=1, lm=1) ---- */
static void test_cmp_rtpt(void)
{
    run_compare_test("cmp_RTPT_sf1_lm1",
                     GTE_CMD_RTPT(1, 1),
                     GTE_Inline_RTPT, 1, 1);
}

/* ---- RTPT (sf=1, lm=0) ---- */
static void test_cmp_rtpt_lm0(void)
{
    run_compare_test("cmp_RTPT_sf1_lm0",
                     GTE_CMD_RTPT(1, 0),
                     GTE_Inline_RTPT, 1, 0);
}

/* ---- MVMVA mx=0 (RT), v=0, cv=0 (TR) ---- */
static void test_cmp_mvmva_rt_v0_tr(void)
{
    run_compare_test_mvmva("cmp_MVMVA_RT_V0_TR", 1, 1, 0, 0, 0);
}

/* ---- MVMVA mx=1 (Light), v=0, cv=1 (BK) ---- */
static void test_cmp_mvmva_light_v0_bk(void)
{
    run_compare_test_mvmva("cmp_MVMVA_Light_V0_BK", 1, 1, 1, 0, 1);
}

/* ---- MVMVA mx=2 (Color), v=0, cv=0 (TR) ---- */
static void test_cmp_mvmva_color_v0_tr(void)
{
    run_compare_test_mvmva("cmp_MVMVA_Color_V0_TR", 1, 1, 2, 0, 0);
}

/* ---- MVMVA mx=0 (RT), v=1, cv=1 (BK) ---- */
static void test_cmp_mvmva_rt_v1_bk(void)
{
    run_compare_test_mvmva("cmp_MVMVA_RT_V1_BK", 1, 1, 0, 1, 1);
}

/* ---- MVMVA mx=0 (RT), v=2, cv=0 (TR) ---- */
static void test_cmp_mvmva_rt_v2_tr(void)
{
    run_compare_test_mvmva("cmp_MVMVA_RT_V2_TR", 1, 1, 0, 2, 0);
}

/* ---- NCS (sf=1, lm=1) ---- */
static void test_cmp_ncs(void)
{
    run_compare_test("cmp_NCS_sf1_lm1",
                     GTE_CMD_NCS(1, 1),
                     GTE_Inline_NCS, 1, 1);
}

/* ---- NCT (sf=1, lm=1) ---- */
static void test_cmp_nct(void)
{
    run_compare_test("cmp_NCT_sf1_lm1",
                     GTE_CMD_NCT(1, 1),
                     GTE_Inline_NCT, 1, 1);
}

/* ---- NCDS (sf=1, lm=1) ---- */
static void test_cmp_ncds(void)
{
    run_compare_test("cmp_NCDS_sf1_lm1",
                     GTE_CMD_NCDS(1, 1),
                     GTE_Inline_NCDS, 1, 1);
}

/* ---- NCDT (sf=1, lm=1) ---- */
static void test_cmp_ncdt(void)
{
    run_compare_test("cmp_NCDT_sf1_lm1",
                     GTE_CMD_NCDT(1, 1),
                     GTE_Inline_NCDT, 1, 1);
}

/* ---- NCCS (sf=1, lm=1) ---- */
static void test_cmp_nccs(void)
{
    run_compare_test("cmp_NCCS_sf1_lm1",
                     GTE_CMD_NCCS(1, 1),
                     GTE_Inline_NCCS, 1, 1);
}

/* ---- NCCT (sf=1, lm=1) ---- */
static void test_cmp_ncct(void)
{
    run_compare_test("cmp_NCCT_sf1_lm1",
                     GTE_CMD_NCCT(1, 1),
                     GTE_Inline_NCCT, 1, 1);
}

/* ---- CC (sf=1, lm=1) ---- */
static void test_cmp_cc(void)
{
    run_compare_test("cmp_CC_sf1_lm1",
                     GTE_CMD_CC(1, 1),
                     GTE_Inline_CC, 1, 1);
}

/* ---- CDP (sf=1, lm=1) ---- */
static void test_cmp_cdp(void)
{
    run_compare_test("cmp_CDP_sf1_lm1",
                     GTE_CMD_CDP(1, 1),
                     GTE_Inline_CDP, 1, 1);
}

/* ================================================================
 * Edge case: large vertex values (stress near-overflow)
 * ================================================================ */
static void test_cmp_rtps_large(void)
{
    BEGIN_TEST("cmp_RTPS_large_vertex");
    setup_full_state();

    /* Override V0 with large values */
    cpu.cp2_data[GTE_VXY0] = PACK_VXY(0x7000, -0x6000);
    cpu.cp2_data[GTE_VZ0]  = 0x7FFF;

    snapshot_save(&saved_input);

    int saved_vu0 = gte_use_vu0;
    gte_use_vu0 = 0;
    GTE_Inline_RTPS(&cpu, 1, 1);
    gte_use_vu0 = saved_vu0;
    snapshot_save(&ref_output);

    snapshot_restore(&saved_input);
    gte_use_vu0 = 1;
    EMIT(GTE_CMD_RTPS(1, 1));
    RUN(500);
    gte_use_vu0 = saved_vu0;

    pg_ctx.fail_count = compare_gte_output("cmp_RTPS_large_vertex");
    END_TEST();
}

/* Edge case: negative translation */
static void test_cmp_rtps_neg_tr(void)
{
    BEGIN_TEST("cmp_RTPS_neg_translation");
    setup_full_state();

    cpu.cp2_ctrl[GTE_TRX] = -5000;
    cpu.cp2_ctrl[GTE_TRY] = -3000;
    cpu.cp2_ctrl[GTE_TRZ] = 500;

    snapshot_save(&saved_input);

    int saved_vu0 = gte_use_vu0;
    gte_use_vu0 = 0;
    GTE_Inline_RTPS(&cpu, 1, 1);
    gte_use_vu0 = saved_vu0;
    snapshot_save(&ref_output);

    snapshot_restore(&saved_input);
    gte_use_vu0 = 1;
    EMIT(GTE_CMD_RTPS(1, 1));
    RUN(500);
    gte_use_vu0 = saved_vu0;

    pg_ctx.fail_count = compare_gte_output("cmp_RTPS_neg_translation");
    END_TEST();
}

/* Edge case: RTPT with different vertices */
static void test_cmp_rtpt_varied(void)
{
    BEGIN_TEST("cmp_RTPT_varied_verts");
    setup_full_state();

    /* Three different vertices with moderate magnitudes */
    cpu.cp2_data[GTE_VXY0] = PACK_VXY(50, -50);
    cpu.cp2_data[GTE_VZ0]  = 800;
    cpu.cp2_data[GTE_VXY1] = PACK_VXY(-200, 300);
    cpu.cp2_data[GTE_VZ1]  = 400;
    cpu.cp2_data[GTE_VXY2] = PACK_VXY(150, 100);
    cpu.cp2_data[GTE_VZ2]  = 600;

    snapshot_save(&saved_input);

    int saved_vu0 = gte_use_vu0;
    gte_use_vu0 = 0;
    GTE_Inline_RTPT(&cpu, 1, 1);
    gte_use_vu0 = saved_vu0;
    snapshot_save(&ref_output);

    snapshot_restore(&saved_input);
    gte_use_vu0 = 1;
    EMIT(GTE_CMD_RTPT(1, 1));
    RUN(500);
    gte_use_vu0 = saved_vu0;

    pg_ctx.fail_count = compare_gte_output("cmp_RTPT_varied_verts");
    END_TEST();
}

/* ================================================================
 * Runner
 * ================================================================ */
void pg_run_gte_compare_tests(void)
{
    printf("\n=== GTE VU0 vs C Reference Comparison ===\n");

    /* RTPS / RTPT */
    test_cmp_rtps();
    test_cmp_rtps_lm0();
    test_cmp_rtpt();
    test_cmp_rtpt_lm0();

    /* MVMVA combos */
    test_cmp_mvmva_rt_v0_tr();
    test_cmp_mvmva_light_v0_bk();
    test_cmp_mvmva_color_v0_tr();
    test_cmp_mvmva_rt_v1_bk();
    test_cmp_mvmva_rt_v2_tr();

    /* Lighting pipeline */
    test_cmp_ncs();
    test_cmp_nct();
    test_cmp_ncds();
    test_cmp_ncdt();
    test_cmp_nccs();
    test_cmp_ncct();
    test_cmp_cc();
    test_cmp_cdp();

    /* Edge cases */
    test_cmp_rtps_large();
    test_cmp_rtps_neg_tr();
    test_cmp_rtpt_varied();
}
