/*
 * JIT Playground — GTE (COP2) Tests
 *
 * Tests GTE operations in the JIT, specifically the interaction between
 * COP2 instructions and dynamic slot dirty tracking.  The pattern that
 * breaks in Crash Bandicoot is:
 *
 *   MTC2 $reg, GTE_data   (write PSX reg to GTE — emit_call_c_lite)
 *   COP2 RTPS             (GTE command — emit_call_c_lite)
 *   MFC2 $reg, GTE_data   (read GTE to PSX reg — emit_call_c_lite)
 *   SW   $reg, offset($base)  (store result — uses dynamic slot)
 *
 * With dirty-only flushing, non-dirty slots are NOT written to cpu.regs[]
 * before lite C calls.  These tests verify that slot values survive
 * GTE operations intact.
 */
#include "playground.h"

/* ---- Helper: enable COP2 in SR ---- */
static void gte_enable_cop2(void)
{
    /* SR bit 30 = CU2 (COP2 usable) must be set.
     * Also set bit 28 = CU0 for good measure. */
    cpu.cop0[PSX_COP0_SR_IDX] = (1u << 30) | (1u << 28);
}

/* ---- Helper: set identity rotation matrix + zero translation ---- */
static void gte_set_identity(void)
{
    /* RT matrix (3x3, fixed 1.3.12):
     * [[4096, 0, 0], [0, 4096, 0], [0, 0, 4096]] = identity
     * Packed as 16-bit pairs in control regs 0-4 */
    cpu.cp2_ctrl[GTE_RT11RT12] = (uint32_t)((0 << 16) | (4096 & 0xFFFF));  /* RT11=4096, RT12=0 */
    cpu.cp2_ctrl[GTE_RT13RT21] = (uint32_t)((0 << 16) | (0 & 0xFFFF));     /* RT13=0, RT21=0 */
    cpu.cp2_ctrl[GTE_RT22RT23] = (uint32_t)((0 << 16) | (4096 & 0xFFFF));  /* RT22=4096, RT23=0 */
    cpu.cp2_ctrl[GTE_RT31RT32] = (uint32_t)((0 << 16) | (0 & 0xFFFF));     /* RT31=0, RT32=0 */
    cpu.cp2_ctrl[GTE_RT33]     = 4096;                                       /* RT33=4096 */

    /* Translation vector = (0, 0, 0) */
    cpu.cp2_ctrl[GTE_TRX] = 0;
    cpu.cp2_ctrl[GTE_TRY] = 0;
    cpu.cp2_ctrl[GTE_TRZ] = 0;

    /* Screen offset: center at (160, 120) in fixed 16.16 */
    cpu.cp2_ctrl[GTE_OFX] = 160 << 16;
    cpu.cp2_ctrl[GTE_OFY] = 120 << 16;

    /* Projection distance H = 256 (arbitrary) */
    cpu.cp2_ctrl[GTE_H] = 256;

    /* DQA/DQB for depth cueing (not tested, but set to avoid div-by-zero) */
    cpu.cp2_ctrl[GTE_DQA] = 0;
    cpu.cp2_ctrl[GTE_DQB] = 0;
}


/* ================================================================
 * Test 1: MTC2 + MFC2 round-trip (simple register transfer)
 *
 * Write a value to GTE data register, read it back.
 * Exercises emit_call_c_lite for both MTC2 and MFC2.
 * The PSX source register ($t1) is in a dynamic slot.
 * ================================================================ */
static void test_gte_mtc2_mfc2_roundtrip(void)
{
    BEGIN_TEST("gte_mtc2_mfc2_roundtrip");
    gte_enable_cop2();

    /* $t1 = 0xDEADBEEF, write to GTE_VXY0, read back to $t2 */
    SET_REG(R_T1, 0xDEADBEEF);
    SET_REG(R_T2, 0);

    EMIT(PSX_MTC2(R_T1, GTE_VXY0));  /* GTE data[0] = $t1 */
    EMIT(PSX_MFC2(R_T2, GTE_VXY0));  /* $t2 = GTE data[0] */

    RUN(200);

    EXPECT_REG(R_T1, 0xDEADBEEF);  /* $t1 should be unchanged */
    EXPECT_REG(R_T2, 0xDEADBEEF);  /* $t2 should have the round-tripped value */
    END_TEST();
}


/* ================================================================
 * Test 2: GTE preserves non-dirty slots
 *
 * Set up several PSX registers (which will become dynamic slots),
 * do a GTE operation (emit_call_c_lite), and verify ALL slot regs
 * survive unchanged.  This is the core dirty-tracking stress test.
 * ================================================================ */
static void test_gte_preserves_slots(void)
{
    BEGIN_TEST("gte_preserves_slots");
    gte_enable_cop2();
    gte_set_identity();

    /* Set up 6 registers that will compete for dynamic slots.
     * After dyn_assign_slots, the highest-frequency ones get slots.
     * We use them all in the block so they get assigned. */
    SET_REG(R_T1, 0x11111111);
    SET_REG(R_T2, 0x22222222);
    SET_REG(R_T3, 0x33333333);
    SET_REG(R_T4, 0x44444444);
    SET_REG(R_T5, 0x55555555);
    SET_REG(R_V0, 0);

    /* Write vertex to GTE via MTC2 (accesses $t1, $t2) */
    EMIT(PSX_MTC2(R_T1, GTE_VXY0));  /* GTE V0.xy = $t1 */
    EMIT(PSX_MTC2(R_T2, GTE_VZ0));   /* GTE V0.z  = $t2 */

    /* Run RTPS (touches GTE internals, calls C via lite trampoline) */
    EMIT(GTE_CMD_RTPS(1, 1));

    /* Read back a GTE result into $v0 */
    EMIT(PSX_MFC2(R_V0, GTE_SXY2));  /* $v0 = screen XY result */

    /* Access $t3, $t4, $t5 to prove they survived the GTE calls.
     * Use ADDU $t3, $t3, $zero to "touch" each register. */
    EMIT(PSX_ADDU(R_T3, R_T3, R_ZERO));  /* $t3 = $t3 (no-op, but reads slot) */
    EMIT(PSX_ADDU(R_T4, R_T4, R_ZERO));  /* $t4 = $t4 */
    EMIT(PSX_ADDU(R_T5, R_T5, R_ZERO));  /* $t5 = $t5 */

    RUN(500);

    /* The key assertions: non-dirty slot values must survive GTE calls */
    EXPECT_REG(R_T1, 0x11111111);  /* was used as MTC2 source (may be non-dirty after flush) */
    EXPECT_REG(R_T2, 0x22222222);
    EXPECT_REG(R_T3, 0x33333333);  /* completely untouched by GTE — MUST survive */
    EXPECT_REG(R_T4, 0x44444444);
    EXPECT_REG(R_T5, 0x55555555);
    END_TEST();
}


/* ================================================================
 * Test 3: GTE + SW pattern (Crash Bandicoot pattern)
 *
 * MTC2 → COP2 RTPS → MFC2 → SW.  The SW writes the GTE result
 * to memory.  With dirty-only, the MFC2 result goes into a slot
 * (dirty), while other slots remain non-dirty.  The SW reads the
 * base register from a (possibly non-dirty) slot.
 * ================================================================ */
static void test_gte_mfc2_then_sw(void)
{
    BEGIN_TEST("gte_mfc2_then_sw");
    gte_enable_cop2();
    gte_set_identity();

    /* $a3 = data area base address (used for SW base) */
    SET_REG(R_A3, PG_DATA_BASE);
    /* Vertex input */
    SET_REG(R_T1, 100 | (200 << 16));  /* VXY0: X=100, Y=200 */
    SET_REG(R_T2, 300);                 /* VZ0: Z=300 */

    /* Write vertex to GTE */
    EMIT(PSX_MTC2(R_T1, GTE_VXY0));
    EMIT(PSX_MTC2(R_T2, GTE_VZ0));

    /* Transform */
    EMIT(GTE_CMD_RTPS(1, 1));

    /* Read result */
    EMIT(PSX_MFC2(R_V0, GTE_SXY2));  /* $v0 = screen XY */

    /* Store result to memory using $a3 as base (non-dirty slot!) */
    EMIT(PSX_SW(R_V0, 0, R_A3));     /* MEM[$a3+0] = $v0 */

    /* Also verify $a3 itself wasn't corrupted */
    EMIT(PSX_SW(R_A3, 4, R_A3));     /* MEM[$a3+4] = $a3 */

    RUN(500);

    /* Check: $a3 should still be the data base address */
    EXPECT_REG(R_A3, PG_DATA_BASE);

    /* Check: the stored value at offset 4 should be the base address */
    EXPECT_MEM32(PG_DATA_OFFSET + 4, PG_DATA_BASE);

    /* Check: MFC2 result was stored to memory (non-zero if GTE worked) */
    uint32_t sxy = GET_MEM32(PG_DATA_OFFSET + 0);
    if (sxy == 0) {
        printf("  [FAIL] %s: GTE SXY2 result is zero (GTE didn't compute?)\n",
               pg_ctx.name);
        pg_ctx.fail_count++;
    }

    END_TEST();
}


/* ================================================================
 * Test 4: Multiple GTE calls in one block
 *
 * Simulates a block with multiple MTC2/MFC2/COP2 sequences.
 * Each GTE call goes through emit_call_c_lite with dirty-only flush.
 * Non-dirty slots must survive ALL calls.
 * ================================================================ */
static void test_gte_multi_call(void)
{
    BEGIN_TEST("gte_multi_call");
    gte_enable_cop2();
    gte_set_identity();

    SET_REG(R_A3, PG_DATA_BASE);
    SET_REG(R_T1, 50 | (60 << 16));   /* vertex 1: X=50, Y=60 */
    SET_REG(R_T2, 100);                /* vertex 1: Z=100 */
    SET_REG(R_T3, 0x12345678);         /* canary value — must survive ALL GTE calls */

    /* --- GTE operation 1 --- */
    EMIT(PSX_MTC2(R_T1, GTE_VXY0));
    EMIT(PSX_MTC2(R_T2, GTE_VZ0));
    EMIT(GTE_CMD_RTPS(1, 1));
    EMIT(PSX_MFC2(R_V0, GTE_SXY2));
    EMIT(PSX_SW(R_V0, 0, R_A3));

    /* --- GTE operation 2 (different vertex, same base register) --- */
    EMIT(PSX_ADDIU(R_T1, R_ZERO, 70 | (80 << 16)));  /* $t1 = new vertex XY */
    EMIT(PSX_ADDIU(R_T2, R_ZERO, 200));                /* $t2 = new Z */
    EMIT(PSX_MTC2(R_T1, GTE_VXY0));
    EMIT(PSX_MTC2(R_T2, GTE_VZ0));
    EMIT(GTE_CMD_RTPS(1, 1));
    EMIT(PSX_MFC2(R_V1, GTE_SXY2));
    EMIT(PSX_SW(R_V1, 8, R_A3));

    /* Store canary to memory to prove it survived */
    EMIT(PSX_SW(R_T3, 16, R_A3));

    RUN(1000);

    /* The critical assertion: canary survived all GTE calls */
    EXPECT_REG(R_T3, 0x12345678);
    EXPECT_MEM32(PG_DATA_OFFSET + 16, 0x12345678);

    /* Base register survived */
    EXPECT_REG(R_A3, PG_DATA_BASE);

    END_TEST();
}


/* ================================================================
 * Test 5: CU2 check must NOT clear dirty bits (regression)
 *
 * REGRESSION TEST for the CU exception dyn_dirty_mask leak bug:
 * Every COP2 instruction has a CU2 usability check that calls
 * emit_call_c(Helper_CU_Exception) inside a BNE-skipped block.
 * emit_call_c clears dyn_dirty_mask at compile time.  Without the
 * save/restore fix, dirty slot values are silently lost on the
 * normal (CU2-enabled) code path.
 *
 * Pattern:
 *   ADDIU $t3, $zero, 0xABCD1234   (dirties $t3 slot)
 *   ADDIU $t4, $zero, 0x56789ABC   (dirties $t4 slot)
 *   MTC2  $t1, GTE_VXY0            (COP2 — CU2 check here)
 *   --- block exit ---
 *   EXPECT_REG($t3, 0xABCD1234)    (reads cpu.regs[$t3])
 *   EXPECT_REG($t4, 0x56789ABC)    (reads cpu.regs[$t4])
 *
 * Without fix: CU2 check clears dirty bits → $t3/$t4 NOT flushed
 *              at block exit → cpu.regs[] has stale prologue values.
 * With fix:    dirty bits preserved → values correctly flushed.
 * ================================================================ */
static void test_gte_cu2_dirty_mask_regression(void)
{
    BEGIN_TEST("gte_cu2_dirty_regression");
    gte_enable_cop2();

    /* Pre-load $t1 for MTC2 source */
    SET_REG(R_T1, 0xDEAD0000);
    /* Pre-load $t3 and $t4 to some OTHER value (the "stale" value) */
    SET_REG(R_T3, 0x00000000);
    SET_REG(R_T4, 0x00000000);

    /* In-block: dirty $t3 and $t4 with new values */
    EMIT(PSX_ADDIU(R_T3, R_ZERO, (int16_t)0x1234));  /* $t3 = 0x00001234 (dirty!) */
    EMIT(PSX_LUI(R_T4, 0x5678));                       /* $t4 = 0x56780000 (dirty!) */

    /* Make $t3/$t4 also have usage count >= 2 so they get dynamic slots */
    EMIT(PSX_ADDU(R_T3, R_T3, R_ZERO));  /* touch $t3 again */
    EMIT(PSX_ADDU(R_T4, R_T4, R_ZERO));  /* touch $t4 again */

    /* COP2 instruction — triggers CU2 check with emit_call_c */
    EMIT(PSX_MTC2(R_T1, GTE_VXY0));

    /* Block ends here.  Epilogue should flush dirty $t3/$t4.
     * Bug: CU2 check's emit_call_c cleared their dirty bits. */

    RUN(200);

    /* These assertions fail without the dyn_dirty_mask save/restore fix */
    EXPECT_REG(R_T3, 0x00001234);
    EXPECT_REG(R_T4, 0x56780000);
    END_TEST();
}


/* ================================================================
 *  Category runner
 * ================================================================ */
void pg_run_gte_tests(void)
{
    printf("--- GTE Tests ---\n");
    test_gte_mtc2_mfc2_roundtrip();
    test_gte_preserves_slots();
    test_gte_mfc2_then_sw();
    test_gte_multi_call();
    test_gte_cu2_dirty_mask_regression();
}
