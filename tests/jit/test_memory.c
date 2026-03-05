/*
 * JIT Playground — Memory Tests
 *
 * Covers: LW/SW, LB/SB, LH/SH, LWL/LWR, SWL/SWR, ISC.
 * 13 tests total.
 */
#include "playground.h"

static void test_lw_sw_basic(void)
{
    BEGIN_TEST("lw_sw_basic");
    EMIT(PSX_LUI(R_T0, 0x8002));
    SET_REG(R_V0, 0xCAFEBABE);
    EMIT(PSX_SW(R_V0, 0, R_T0));
    EMIT(PSX_LW(R_A0, 0, R_T0));
    RUN(2000);
    EXPECT_REG(R_A0, 0xCAFEBABE);
    EXPECT_MEM32(PG_DATA_OFFSET, 0xCAFEBABE);
    END_TEST();
}

static void test_lb_sb_signext(void)
{
    BEGIN_TEST("lb_sb_signext");
    EMIT(PSX_LUI(R_T0, 0x8002));
    SET_REG(R_V0, 0xFF);
    EMIT(PSX_SB(R_V0, 0, R_T0));
    EMIT(PSX_LB(R_A0, 0, R_T0));
    EMIT(PSX_LBU(R_A1, 0, R_T0));
    RUN(2000);
    EXPECT_REG(R_A0, 0xFFFFFFFF);
    EXPECT_REG(R_A1, 0x000000FF);
    END_TEST();
}

static void test_lh_sh(void)
{
    BEGIN_TEST("lh_sh");
    EMIT(PSX_LUI(R_T0, 0x8002));
    SET_REG(R_V0, 0x8001);
    EMIT(PSX_SH(R_V0, 0, R_T0));
    EMIT(PSX_LH(R_A0, 0, R_T0));
    EMIT(PSX_LHU(R_A1, 0, R_T0));
    RUN(2000);
    EXPECT_REG(R_A0, 0xFFFF8001);
    EXPECT_REG(R_A1, 0x00008001);
    END_TEST();
}

static void test_sw_lw_offset(void)
{
    BEGIN_TEST("sw_lw_offset");
    EMIT(PSX_LUI(R_T0, 0x8002));
    SET_REG(R_V0, 0x11111111);
    SET_REG(R_V1, 0x22222222);
    EMIT(PSX_SW(R_V0, 0, R_T0));
    EMIT(PSX_SW(R_V1, 4, R_T0));
    EMIT(PSX_LW(R_A0, 0, R_T0));
    EMIT(PSX_LW(R_A1, 4, R_T0));
    RUN(2000);
    EXPECT_REG(R_A0, 0x11111111);
    EXPECT_REG(R_A1, 0x22222222);
    END_TEST();
}

static void test_lwl_lwr(void)
{
    BEGIN_TEST("lwl_lwr");
    SET_MEM32(PG_DATA_OFFSET, 0xAABBCCDD);
    EMIT(PSX_LUI(R_T0, 0x8002));
    SET_REG(R_A0, 0);
    EMIT(PSX_LWL(R_A0, 3, R_T0));
    EMIT(PSX_LWR(R_A0, 0, R_T0));
    RUN(2000);
    EXPECT_REG(R_A0, 0xAABBCCDD);
    END_TEST();
}

static void test_swl_swr(void)
{
    BEGIN_TEST("swl_swr");
    SET_MEM32(PG_DATA_OFFSET, 0);
    EMIT(PSX_LUI(R_T0, 0x8002));
    SET_REG(R_V0, 0x12345678);
    EMIT(PSX_SWL(R_V0, 3, R_T0));
    EMIT(PSX_SWR(R_V0, 0, R_T0));
    EMIT(PSX_LW(R_A0, 0, R_T0));
    RUN(2000);
    EXPECT_REG(R_A0, 0x12345678);
    EXPECT_MEM32(PG_DATA_OFFSET, 0x12345678);
    END_TEST();
}

/* ================================================================
 *  ISC (Cache Isolation) Tests
 *
 *  When SR.IsC (bit 16) is set, stores to KUSEG/KSEG0 must be silently
 *  dropped — the BIOS uses this for I-cache flush. Tests cover:
 *   - SW with ISC=0 → normal write
 *   - SW with ISC=1 → write silently dropped
 *   - SB with ISC=1 → write silently dropped
 *   - SH with ISC=1 → write silently dropped
 *   - MTC0 setting ISC=1 then SW in same block → dropped
 *   - MTC0 clearing ISC=0 then SW in same block → written
 * ================================================================ */

/* Verify MFC0 reads COP0 SR correctly — baseline for ISC tests */
static void test_mfc0_read_sr(void)
{
    BEGIN_TEST("mfc0_read_sr");
    SET_COP0(PSX_COP0_SR, 0x00010000);        /* ISC=1 */

    EMIT(PSX_MFC0(R_V0, PSX_COP0_SR_IDX));    /* v0 = cop0[12] */
    RUN(2000);

    EXPECT_REG(R_V0, 0x00010000);
    END_TEST();
}

/* ISC=0: store goes through (baseline — same as test_lw_sw_basic essentially) */
static void test_sw_isc_clear(void)
{
    BEGIN_TEST("sw_isc_clear");
    /* SR with ISC=0 (bit 16 clear) */
    SET_COP0(PSX_COP0_SR, 0x00000000);
    SET_MEM32(PG_DATA_OFFSET, 0x00000000);

    EMIT(PSX_LUI(R_T0, 0x8002));       /* t0 = 0x80020000 (data area) */
    SET_REG(R_V0, 0xDEADBEEF);
    EMIT(PSX_SW(R_V0, 0, R_T0));       /* SW v0, 0(t0) */
    EMIT(PSX_LW(R_A0, 0, R_T0));       /* LW a0, 0(t0) — read back */
    RUN(2000);

    EXPECT_REG(R_A0, 0xDEADBEEF);
    EXPECT_MEM32(PG_DATA_OFFSET, 0xDEADBEEF);
    END_TEST();
}

/* ISC=1: SW must be silently dropped — memory unchanged */
static void test_sw_isc_set(void)
{
    BEGIN_TEST("sw_isc_set");
    /* Use emitted MTC0 to set ISC — block must contain MTC0 SR so the
     * const-address fast path is bypassed and ISC checks apply. */
    SET_COP0(PSX_COP0_SR, 0x00000000);        /* start ISC=0 */
    SET_MEM32(PG_DATA_OFFSET, 0xAAAAAAAA);     /* sentinel */

    EMIT(PSX_LUI(R_T0, 0x8002));
    /* Set ISC=1 via MTC0 */
    SET_REG(R_T1, 0x00010000);
    EMIT(PSX_MTC0(R_T1, PSX_COP0_SR_IDX));
    SET_REG(R_V0, 0xDEADBEEF);
    EMIT(PSX_SW(R_V0, 0, R_T0));              /* should be dropped */
    RUN(2000);

    /* Memory must be unchanged */
    EXPECT_MEM32(PG_DATA_OFFSET, 0xAAAAAAAA);
    /* Restore ISC=0 for next test */
    SET_COP0(PSX_COP0_SR, 0x00000000);
    END_TEST();
}

/* ISC=1: SB must also be dropped */
static void test_sb_isc_set(void)
{
    BEGIN_TEST("sb_isc_set");
    SET_COP0(PSX_COP0_SR, 0x00000000);
    SET_MEM8(PG_DATA_OFFSET, 0xBB);           /* sentinel */

    EMIT(PSX_LUI(R_T0, 0x8002));
    SET_REG(R_T1, 0x00010000);
    EMIT(PSX_MTC0(R_T1, PSX_COP0_SR_IDX));
    SET_REG(R_V0, 0x55);
    EMIT(PSX_SB(R_V0, 0, R_T0));             /* should be dropped */
    RUN(2000);

    EXPECT_MEM8(PG_DATA_OFFSET, 0xBB);
    SET_COP0(PSX_COP0_SR, 0x00000000);
    END_TEST();
}

/* ISC=1: SH must also be dropped */
static void test_sh_isc_set(void)
{
    BEGIN_TEST("sh_isc_set");
    SET_COP0(PSX_COP0_SR, 0x00000000);
    SET_MEM16(PG_DATA_OFFSET, 0xCCDD);        /* sentinel */

    EMIT(PSX_LUI(R_T0, 0x8002));
    SET_REG(R_T1, 0x00010000);
    EMIT(PSX_MTC0(R_T1, PSX_COP0_SR_IDX));
    SET_REG(R_V0, 0x1234);
    EMIT(PSX_SH(R_V0, 0, R_T0));             /* should be dropped */
    RUN(2000);

    EXPECT_MEM16(PG_DATA_OFFSET, 0xCCDD);
    SET_COP0(PSX_COP0_SR, 0x00000000);
    END_TEST();
}

/* Block contains MTC0 setting ISC=1, then SW — SW must be dropped.
 * Since the block has MTC0 to SR, it uses inline ISC check (5 words)
 * instead of the cached path. */
static void test_sw_mtc0_set_isc(void)
{
    BEGIN_TEST("sw_mtc0_set_isc");
    SET_COP0(PSX_COP0_SR, 0x00000000);        /* start ISC=0 */
    SET_MEM32(PG_DATA_OFFSET, 0xAAAAAAAA);

    EMIT(PSX_LUI(R_T0, 0x8002));              /* t0 = data area */
    /* Set ISC=1 via MTC0 */
    SET_REG(R_V1, 0x00010000);                 /* v1 = 0x10000 (ISC bit) */
    EMIT(PSX_MTC0(R_V1, PSX_COP0_SR_IDX));    /* MTC0 v1, SR → ISC=1 */
    /* Now store — should be dropped because ISC just set */
    SET_REG(R_V0, 0xDEADBEEF);
    EMIT(PSX_SW(R_V0, 0, R_T0));
    RUN(2000);

    /* Memory should remain unchanged */
    EXPECT_MEM32(PG_DATA_OFFSET, 0xAAAAAAAA);
    END_TEST();
}

/* Block contains MTC0 clearing ISC=0, then SW — SW must go through.
 * This tests the inline path where MTC0 changes SR mid-block. */
static void test_sw_mtc0_clear_isc(void)
{
    BEGIN_TEST("sw_mtc0_clear_isc");
    SET_COP0(PSX_COP0_SR, 0x00010000);        /* start ISC=1 */
    SET_MEM32(PG_DATA_OFFSET, 0xAAAAAAAA);

    EMIT(PSX_LUI(R_T0, 0x8002));              /* t0 = data area */
    /* Clear ISC via MTC0 */
    SET_REG(R_V1, 0x00000000);                 /* v1 = 0 (ISC cleared) */
    EMIT(PSX_MTC0(R_V1, PSX_COP0_SR_IDX));    /* MTC0 v1, SR → ISC=0 */
    /* Now store — should go through because ISC cleared */
    SET_REG(R_V0, 0xDEADBEEF);
    EMIT(PSX_SW(R_V0, 0, R_T0));
    EMIT(PSX_LW(R_A0, 0, R_T0));              /* read back */
    RUN(2000);

    EXPECT_REG(R_A0, 0xDEADBEEF);
    EXPECT_MEM32(PG_DATA_OFFSET, 0xDEADBEEF);
    END_TEST();
}

/* ================================================================
 *  Category Runner
 * ================================================================ */

void pg_run_memory_tests(void)
{
    printf("\n--- Load / Store ---\n");
    test_lw_sw_basic();
    test_lb_sb_signext();
    test_lh_sh();
    test_sw_lw_offset();
    test_lwl_lwr();
    test_swl_swr();

    printf("\n--- ISC (Cache Isolation) ---\n");
    test_mfc0_read_sr();
    test_sw_isc_clear();
    test_sw_isc_set();
    test_sb_isc_set();
    test_sh_isc_set();
    test_sw_mtc0_set_isc();
    test_sw_mtc0_clear_isc();
}
