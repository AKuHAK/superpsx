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
 * Test 6: NCLIP basic — counter-clockwise triangle → positive result
 *
 * SXY0=(0,0), SXY1=(100,0), SXY2=(0,100)
 * MAC0 = 0*(0-100) + 100*(100-0) + 0*(0-0) = 10000
 * FLAG = 0 (no overflow for small screen coords)
 * ================================================================ */
static void test_gte_nclip_positive(void)
{
    BEGIN_TEST("gte_nclip_positive");
    gte_enable_cop2();

    /* Set SXY vertices directly in cp2_data */
    cpu.cp2_data[GTE_SXY0] = PACK_SXY(0, 0);
    cpu.cp2_data[GTE_SXY1] = PACK_SXY(100, 0);
    cpu.cp2_data[GTE_SXY2] = PACK_SXY(0, 100);
    cpu.cp2_data[GTE_MAC0] = 0xDEADBEEF;  /* canary */
    cpu.cp2_ctrl[GTE_FLAG_CTRL] = 0xDEADBEEF;  /* canary */

    /* Emit NCLIP command */
    EMIT(GTE_CMD_NCLIP);

    RUN(200);

    /* MAC0 should be 10000 */
    EXPECT_CP2_DATA(GTE_MAC0, 10000);
    /* FLAG should be 0 (no overflow) */
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 7: NCLIP — clockwise triangle → negative result
 *
 * SXY0=(0,0), SXY1=(0,100), SXY2=(100,0)
 * MAC0 = 0*(100-0) + 0*(0-0) + 100*(0-100) = -10000
 * ================================================================ */
static void test_gte_nclip_negative(void)
{
    BEGIN_TEST("gte_nclip_negative");
    gte_enable_cop2();

    cpu.cp2_data[GTE_SXY0] = PACK_SXY(0, 0);
    cpu.cp2_data[GTE_SXY1] = PACK_SXY(0, 100);
    cpu.cp2_data[GTE_SXY2] = PACK_SXY(100, 0);

    EMIT(GTE_CMD_NCLIP);

    RUN(200);

    /* -10000 as uint32 = 0xFFFFD8F0 */
    EXPECT_CP2_DATA(GTE_MAC0, (uint32_t)(int32_t)-10000);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 8: NCLIP — collinear points → zero result
 *
 * SXY0=(0,0), SXY1=(10,10), SXY2=(20,20)
 * MAC0 = 0*(10-20) + 10*(20-0) + 20*(0-10) = 0 + 200 - 200 = 0
 * ================================================================ */
static void test_gte_nclip_zero(void)
{
    BEGIN_TEST("gte_nclip_zero");
    gte_enable_cop2();

    cpu.cp2_data[GTE_SXY0] = PACK_SXY(0, 0);
    cpu.cp2_data[GTE_SXY1] = PACK_SXY(10, 10);
    cpu.cp2_data[GTE_SXY2] = PACK_SXY(20, 20);
    cpu.cp2_data[GTE_MAC0] = 0xDEADBEEF;

    EMIT(GTE_CMD_NCLIP);

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC0, 0);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 9: NCLIP — negative screen coordinates
 *
 * SXY0=(-100,-50), SXY1=(100,-50), SXY2=(0,100)
 * MAC0 = -100*(-50-100) + 100*(100-(-50)) + 0*(-50-(-50))
 *       = -100*(-150) + 100*(150) + 0
 *       = 15000 + 15000 = 30000
 * ================================================================ */
static void test_gte_nclip_negcoords(void)
{
    BEGIN_TEST("gte_nclip_negcoords");
    gte_enable_cop2();

    cpu.cp2_data[GTE_SXY0] = PACK_SXY(-100, -50);
    cpu.cp2_data[GTE_SXY1] = PACK_SXY(100, -50);
    cpu.cp2_data[GTE_SXY2] = PACK_SXY(0, 100);

    EMIT(GTE_CMD_NCLIP);

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC0, 30000);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 10: NCLIP — MFC2 readback of MAC0
 *
 * Verify the NCLIP result can be read via MFC2 into a GPR,
 * which is the real-world usage pattern.
 * ================================================================ */
static void test_gte_nclip_mfc2_readback(void)
{
    BEGIN_TEST("gte_nclip_mfc2_readback");
    gte_enable_cop2();

    cpu.cp2_data[GTE_SXY0] = PACK_SXY(0, 0);
    cpu.cp2_data[GTE_SXY1] = PACK_SXY(100, 0);
    cpu.cp2_data[GTE_SXY2] = PACK_SXY(0, 100);

    SET_REG(R_V0, 0);

    /* NCLIP → MFC2 readback → store to memory */
    EMIT(GTE_CMD_NCLIP);
    EMIT(PSX_MFC2(R_V0, GTE_MAC0));

    RUN(200);

    EXPECT_REG(R_V0, 10000);
    END_TEST();
}


/* ================================================================
 * Test 11: AVSZ3 — Average of 3 Z values
 *
 * ZSF3=1365(≈4096/3), SZ1=1000, SZ2=2000, SZ3=3000
 * MAC0 = ZSF3*(SZ1+SZ2+SZ3) = 1365*6000 = 8190000
 * OTZ  = MAC0 / 4096 = 8190000>>12 = 1999
 * FLAG = 0
 * ================================================================ */
static void test_gte_avsz3(void)
{
    BEGIN_TEST("gte_avsz3");
    gte_enable_cop2();

    cpu.cp2_ctrl[GTE_ZSF3] = 1365;
    cpu.cp2_data[GTE_SZ1]  = 1000;
    cpu.cp2_data[GTE_SZ2]  = 2000;
    cpu.cp2_data[GTE_SZ3]  = 3000;
    cpu.cp2_data[GTE_MAC0] = 0xDEADBEEF;
    cpu.cp2_data[GTE_OTZ]  = 0xDEAD;

    EMIT(GTE_CMD_AVSZ3);

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC0, 8190000);
    EXPECT_CP2_DATA(GTE_OTZ, 1999);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 12: AVSZ4 — Average of 4 Z values
 *
 * ZSF4=1024(≈4096/4), SZ0=500, SZ1=1000, SZ2=1500, SZ3=2000
 * MAC0 = ZSF4*(SZ0+SZ1+SZ2+SZ3) = 1024*5000 = 5120000
 * OTZ  = 5120000>>12 = 1250
 * FLAG = 0
 * ================================================================ */
static void test_gte_avsz4(void)
{
    BEGIN_TEST("gte_avsz4");
    gte_enable_cop2();

    cpu.cp2_ctrl[GTE_ZSF4] = 1024;
    cpu.cp2_data[GTE_SZ0]  = 500;
    cpu.cp2_data[GTE_SZ1]  = 1000;
    cpu.cp2_data[GTE_SZ2]  = 1500;
    cpu.cp2_data[GTE_SZ3]  = 2000;

    EMIT(GTE_CMD_AVSZ4);

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC0, 5120000);
    EXPECT_CP2_DATA(GTE_OTZ, 1250);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 13: OP — Outer Product (cross product of RT diag × IR)
 *
 * sf=1, lm=0
 * D1=RT11=4096, D2=RT22=4096, D3=RT33=4096
 * IR=(100, 200, 300)
 * MAC1 = (D2*IR3 - D3*IR2) >> 12 = (4096*300-4096*200)>>12 = 100
 * MAC2 = (D3*IR1 - D1*IR3) >> 12 = (4096*100-4096*300)>>12 = -200
 * MAC3 = (D1*IR2 - D2*IR1) >> 12 = (4096*200-4096*100)>>12 = 100
 * ================================================================ */
static void test_gte_op(void)
{
    BEGIN_TEST("gte_op");
    gte_enable_cop2();

    /* Identity rotation matrix — diag = (4096, 4096, 4096) */
    cpu.cp2_ctrl[GTE_RT11RT12] = (0 << 16) | (4096 & 0xFFFF);
    cpu.cp2_ctrl[GTE_RT13RT21] = 0;
    cpu.cp2_ctrl[GTE_RT22RT23] = (0 << 16) | (4096 & 0xFFFF);
    cpu.cp2_ctrl[GTE_RT31RT32] = 0;
    cpu.cp2_ctrl[GTE_RT33]     = 4096;

    cpu.cp2_data[GTE_IR1] = 100;
    cpu.cp2_data[GTE_IR2] = 200;
    cpu.cp2_data[GTE_IR3] = 300;

    EMIT(GTE_CMD_OP(1, 0));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 100);
    EXPECT_CP2_DATA(GTE_MAC2, (uint32_t)(int32_t)-200);
    EXPECT_CP2_DATA(GTE_MAC3, 100);
    EXPECT_CP2_DATA(GTE_IR1, 100);
    EXPECT_CP2_DATA(GTE_IR2, (uint32_t)(int16_t)-200);
    EXPECT_CP2_DATA(GTE_IR3, 100);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 14: SQR — Square of IR vector (sf=0, no shift)
 *
 * sf=0, lm=0: IR1-3 squared, no shift
 * IR1=10, IR2=20, IR3=30
 * MAC1 = 10*10 = 100, MAC2 = 20*20 = 400, MAC3 = 30*30 = 900
 * IR1=100, IR2=400, IR3=900
 * ================================================================ */
static void test_gte_sqr_sf0(void)
{
    BEGIN_TEST("gte_sqr_sf0");
    gte_enable_cop2();

    cpu.cp2_data[GTE_IR1] = 10;
    cpu.cp2_data[GTE_IR2] = 20;
    cpu.cp2_data[GTE_IR3] = 30;

    EMIT(GTE_CMD_SQR(0, 0));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 100);
    EXPECT_CP2_DATA(GTE_MAC2, 400);
    EXPECT_CP2_DATA(GTE_MAC3, 900);
    EXPECT_CP2_DATA(GTE_IR1, 100);
    EXPECT_CP2_DATA(GTE_IR2, 400);
    EXPECT_CP2_DATA(GTE_IR3, 900);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 15: SQR — Square of IR vector (sf=1, >>12 shift)
 *
 * sf=1, lm=0: IR1-3 squared then >>12
 * IR1=100, IR2=200, IR3=300
 * MAC1 = (100*100)>>12 = 10000>>12 = 2
 * MAC2 = (200*200)>>12 = 40000>>12 = 9
 * MAC3 = (300*300)>>12 = 90000>>12 = 21
 * ================================================================ */
static void test_gte_sqr_sf1(void)
{
    BEGIN_TEST("gte_sqr_sf1");
    gte_enable_cop2();

    cpu.cp2_data[GTE_IR1] = 100;
    cpu.cp2_data[GTE_IR2] = 200;
    cpu.cp2_data[GTE_IR3] = 300;

    EMIT(GTE_CMD_SQR(1, 0));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 2);
    EXPECT_CP2_DATA(GTE_MAC2, 9);
    EXPECT_CP2_DATA(GTE_MAC3, 21);
    EXPECT_CP2_DATA(GTE_IR1, 2);
    EXPECT_CP2_DATA(GTE_IR2, 9);
    EXPECT_CP2_DATA(GTE_IR3, 21);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 16: GPF — General Purpose Interpolation (sf=1, lm=0)
 *
 * IR0=4096 (1.0), IR1=100, IR2=200, IR3=300
 * MAC1 = (IR1*IR0)>>12 = (100*4096)>>12 = 100
 * MAC2 = (IR2*IR0)>>12 = (200*4096)>>12 = 200
 * MAC3 = (IR3*IR0)>>12 = (300*4096)>>12 = 300
 * push_color: r=100>>4=6, g=200>>4=12, b=300>>4=18
 * RGB2 = 6 | (12<<8) | (18<<16) | (code<<24)
 *       with code=0 → 0x00120C06
 * ================================================================ */
static void test_gte_gpf(void)
{
    BEGIN_TEST("gte_gpf");
    gte_enable_cop2();

    cpu.cp2_data[GTE_IR0] = 0x1000;  /* 4096 = 1.0 in 1.3.12 */
    cpu.cp2_data[GTE_IR1] = 100;
    cpu.cp2_data[GTE_IR2] = 200;
    cpu.cp2_data[GTE_IR3] = 300;
    cpu.cp2_data[GTE_RGBC] = 0;      /* code=0 */
    cpu.cp2_data[GTE_RGB0] = 0xAA;
    cpu.cp2_data[GTE_RGB1] = 0xBB;
    cpu.cp2_data[GTE_RGB2] = 0xCC;

    EMIT(GTE_CMD_GPF(1, 0));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 100);
    EXPECT_CP2_DATA(GTE_MAC2, 200);
    EXPECT_CP2_DATA(GTE_MAC3, 300);
    EXPECT_CP2_DATA(GTE_IR1, 100);
    EXPECT_CP2_DATA(GTE_IR2, 200);
    EXPECT_CP2_DATA(GTE_IR3, 300);
    /* RGB FIFO shift: RGB0←old_RGB1, RGB1←old_RGB2 */
    EXPECT_CP2_DATA(GTE_RGB0, 0xBB);
    EXPECT_CP2_DATA(GTE_RGB1, 0xCC);
    /* RGB2 = new color */
    EXPECT_CP2_DATA(GTE_RGB2, 0x00120C06);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 17: GPL — General Purpose Interpolation with Base (sf=1, lm=0)
 *
 * IR0=4096 (1.0), IR1=100, IR2=200, IR3=300
 * MAC1=10, MAC2=20, MAC3=30 (base values, get <<12)
 * result1 = IR1*IR0 + MAC1<<12 = 100*4096 + 10*4096 = 450560
 * MAC1 = 450560>>12 = 110
 * result2 = IR2*IR0 + MAC2<<12 = 200*4096 + 20*4096 = 901120
 * MAC2 = 901120>>12 = 220
 * result3 = IR3*IR0 + MAC3<<12 = 300*4096 + 30*4096 = 1351680
 * MAC3 = 1351680>>12 = 330
 * push_color: r=110>>4=6, g=220>>4=13, b=330>>4=20
 * RGB2 = 6 | (13<<8) | (20<<16) = 0x00140D06
 * ================================================================ */
static void test_gte_gpl(void)
{
    BEGIN_TEST("gte_gpl");
    gte_enable_cop2();

    cpu.cp2_data[GTE_IR0]  = 0x1000;
    cpu.cp2_data[GTE_IR1]  = 100;
    cpu.cp2_data[GTE_IR2]  = 200;
    cpu.cp2_data[GTE_IR3]  = 300;
    cpu.cp2_data[GTE_MAC1] = 10;
    cpu.cp2_data[GTE_MAC2] = 20;
    cpu.cp2_data[GTE_MAC3] = 30;
    cpu.cp2_data[GTE_RGBC] = 0;
    cpu.cp2_data[GTE_RGB0] = 0xAA;
    cpu.cp2_data[GTE_RGB1] = 0xBB;
    cpu.cp2_data[GTE_RGB2] = 0xCC;

    EMIT(GTE_CMD_GPL(1, 0));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 110);
    EXPECT_CP2_DATA(GTE_MAC2, 220);
    EXPECT_CP2_DATA(GTE_MAC3, 330);
    EXPECT_CP2_DATA(GTE_IR1, 110);
    EXPECT_CP2_DATA(GTE_IR2, 220);
    EXPECT_CP2_DATA(GTE_IR3, 330);
    EXPECT_CP2_DATA(GTE_RGB2, 0x00140D06);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 18: DPCS — Depth Cueing Single (sf=1, lm=0)
 *
 * RGBC = black (0,0,0), IR0 = 2048 (0.5×)
 * RFC=GFC=BFC = 240
 * acc = 0, fc = 240<<12 = 983040
 * d = 983040, tmp_ir = 240
 * result = 240*2048 + 0 = 491520
 * MAC = 491520>>12 = 120, IR = 120
 * color = 120>>4 = 7
 * RGB2 = 0x00070707
 * ================================================================ */
static void test_gte_dpcs(void)
{
    BEGIN_TEST("gte_dpcs");
    gte_enable_cop2();

    cpu.cp2_data[GTE_RGBC] = 0x00000000;  /* black */
    cpu.cp2_data[GTE_IR0]  = 0x800;       /* 2048 = 0.5 */
    cpu.cp2_ctrl[GTE_RFC]  = 240;
    cpu.cp2_ctrl[GTE_GFC]  = 240;
    cpu.cp2_ctrl[GTE_BFC]  = 240;
    cpu.cp2_data[GTE_RGB0] = 0;
    cpu.cp2_data[GTE_RGB1] = 0;
    cpu.cp2_data[GTE_RGB2] = 0;

    EMIT(GTE_CMD_DPCS(1, 0));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 120);
    EXPECT_CP2_DATA(GTE_MAC2, 120);
    EXPECT_CP2_DATA(GTE_MAC3, 120);
    EXPECT_CP2_DATA(GTE_IR1, 120);
    EXPECT_CP2_DATA(GTE_IR2, 120);
    EXPECT_CP2_DATA(GTE_IR3, 120);
    EXPECT_CP2_DATA(GTE_RGB2, 0x00070707);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 19: INTPL — Interpolation (sf=1, lm=0)
 *
 * IR0=2048 (0.5), IR1=100, IR2=200, IR3=300
 * RFC=GFC=BFC=200
 * acc = IR << 12 → (409600, 819200, 1228800)
 * fc = 200<<12 = 819200
 * d1 = 819200-409600 = 409600 → tmp_ir=100
 * d2 = 819200-819200 = 0 → tmp_ir=0
 * d3 = 819200-1228800 = -409600 → tmp_ir=-100
 * r1 = 100*2048+409600=614400 → MAC1=150
 * r2 = 0*2048+819200=819200 → MAC2=200
 * r3 = -100*2048+1228800=1024000 → MAC3=250
 * color: 9,12,15 → RGB2 = 0x000F0C09
 * ================================================================ */
static void test_gte_intpl(void)
{
    BEGIN_TEST("gte_intpl");
    gte_enable_cop2();

    cpu.cp2_data[GTE_IR0] = 0x800;
    cpu.cp2_data[GTE_IR1] = 100;
    cpu.cp2_data[GTE_IR2] = 200;
    cpu.cp2_data[GTE_IR3] = 300;
    cpu.cp2_ctrl[GTE_RFC] = 200;
    cpu.cp2_ctrl[GTE_GFC] = 200;
    cpu.cp2_ctrl[GTE_BFC] = 200;
    cpu.cp2_data[GTE_RGBC] = 0;
    cpu.cp2_data[GTE_RGB0] = 0;
    cpu.cp2_data[GTE_RGB1] = 0;
    cpu.cp2_data[GTE_RGB2] = 0;

    EMIT(GTE_CMD_INTPL(1, 0));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 150);
    EXPECT_CP2_DATA(GTE_MAC2, 200);
    EXPECT_CP2_DATA(GTE_MAC3, 250);
    EXPECT_CP2_DATA(GTE_IR1, 150);
    EXPECT_CP2_DATA(GTE_IR2, 200);
    EXPECT_CP2_DATA(GTE_IR3, 250);
    EXPECT_CP2_DATA(GTE_RGB2, 0x000F0C09);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 20: DCPL — Depth Cue Color Light (sf=1, lm=0)
 *
 * RGBC: R=32, G=64, B=128 → 0x00804020
 * IR0=2048, IR1=IR2=IR3=256
 * RFC=GFC=BFC=200
 * acc1=(32*256)<<4=131072, acc2=(64*256)<<4=262144,
 * acc3=(128*256)<<4=524288
 * fc=200<<12=819200
 * d1=688128→tmp=168, d2=557056→tmp=136, d3=294912→tmp=72
 * r1=168*2048+131072=475136→MAC1=116
 * r2=136*2048+262144=540672→MAC2=132
 * r3=72*2048+524288=671744→MAC3=164
 * color: 7,8,10 → RGB2=0x000A0807
 * ================================================================ */
static void test_gte_dcpl(void)
{
    BEGIN_TEST("gte_dcpl");
    gte_enable_cop2();

    cpu.cp2_data[GTE_RGBC] = 0x00804020;  /* R=32, G=64, B=128 */
    cpu.cp2_data[GTE_IR0]  = 0x800;
    cpu.cp2_data[GTE_IR1]  = 256;
    cpu.cp2_data[GTE_IR2]  = 256;
    cpu.cp2_data[GTE_IR3]  = 256;
    cpu.cp2_ctrl[GTE_RFC]  = 200;
    cpu.cp2_ctrl[GTE_GFC]  = 200;
    cpu.cp2_ctrl[GTE_BFC]  = 200;
    cpu.cp2_data[GTE_RGB0] = 0;
    cpu.cp2_data[GTE_RGB1] = 0;
    cpu.cp2_data[GTE_RGB2] = 0;

    EMIT(GTE_CMD_DCPL(1, 0));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 116);
    EXPECT_CP2_DATA(GTE_MAC2, 132);
    EXPECT_CP2_DATA(GTE_MAC3, 164);
    EXPECT_CP2_DATA(GTE_IR1, 116);
    EXPECT_CP2_DATA(GTE_IR2, 132);
    EXPECT_CP2_DATA(GTE_IR3, 164);
    EXPECT_CP2_DATA(GTE_RGB2, 0x000A0807);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 21: DPCT — Depth Cueing Triple (sf=1, lm=0)
 *
 * All RGB FIFO entries = black (0,0,0)
 * IR0=2048, RFC=GFC=BFC=240
 * Each iteration: acc=0, fc=983040, d=983040, tmp_ir=240
 * result=240*2048=491520→MAC=120→IR=120, color=7→0x00070707
 * After 3 iterations: all RGB FIFO = 0x00070707
 * ================================================================ */
static void test_gte_dpct(void)
{
    BEGIN_TEST("gte_dpct");
    gte_enable_cop2();

    cpu.cp2_data[GTE_RGB0] = 0x00000000;
    cpu.cp2_data[GTE_RGB1] = 0x00000000;
    cpu.cp2_data[GTE_RGB2] = 0x00000000;
    cpu.cp2_data[GTE_IR0]  = 0x800;
    cpu.cp2_ctrl[GTE_RFC]  = 240;
    cpu.cp2_ctrl[GTE_GFC]  = 240;
    cpu.cp2_ctrl[GTE_BFC]  = 240;
    cpu.cp2_data[GTE_RGBC] = 0;

    EMIT(GTE_CMD_DPCT(1, 0));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 120);
    EXPECT_CP2_DATA(GTE_MAC2, 120);
    EXPECT_CP2_DATA(GTE_MAC3, 120);
    EXPECT_CP2_DATA(GTE_RGB0, 0x00070707);
    EXPECT_CP2_DATA(GTE_RGB1, 0x00070707);
    EXPECT_CP2_DATA(GTE_RGB2, 0x00070707);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ---- Helper: set identity Light matrix ---- */
static void gte_set_identity_light(void)
{
    cpu.cp2_ctrl[GTE_L11L12] = (0 << 16) | (4096 & 0xFFFF);
    cpu.cp2_ctrl[GTE_L13L21] = 0;
    cpu.cp2_ctrl[GTE_L22L23] = (0 << 16) | (4096 & 0xFFFF);
    cpu.cp2_ctrl[GTE_L31L32] = 0;
    cpu.cp2_ctrl[GTE_L33]    = 4096;
}

/* ---- Helper: set identity Color matrix ---- */
static void gte_set_identity_color(void)
{
    cpu.cp2_ctrl[GTE_LR1LR2] = (0 << 16) | (4096 & 0xFFFF);
    cpu.cp2_ctrl[GTE_LR3LG1] = 0;
    cpu.cp2_ctrl[GTE_LG2LG3] = (0 << 16) | (4096 & 0xFFFF);
    cpu.cp2_ctrl[GTE_LB1LB2] = 0;
    cpu.cp2_ctrl[GTE_LB3]    = 4096;
}

/* ---- Helper: set zero background color ---- */
static void gte_set_zero_bk(void)
{
    cpu.cp2_ctrl[GTE_RBK] = 0;
    cpu.cp2_ctrl[GTE_GBK] = 0;
    cpu.cp2_ctrl[GTE_BBK] = 0;
}


/* ================================================================
 * Test 22: MVMVA — Matrix-Vector Multiply Add
 *         (sf=1, lm=0, mx=0/RT, v=0/V0, cv=0/TR)
 *
 * Identity RT, TR=(100,200,300), V0=(10,20,30)
 * m1 = (100<<12) + 4096*10 = 409600+40960 = 450560
 * MAC1 = 450560>>12 = 110
 * m2 = (200<<12) + 4096*20 = 819200+81920 = 901120
 * MAC2 = 901120>>12 = 220
 * m3 = (300<<12) + 4096*30 = 1228800+122880 = 1351680
 * MAC3 = 1351680>>12 = 330
 * ================================================================ */
static void test_gte_mvmva(void)
{
    BEGIN_TEST("gte_mvmva");
    gte_enable_cop2();

    /* Identity RT */
    cpu.cp2_ctrl[GTE_RT11RT12] = (0 << 16) | (4096 & 0xFFFF);
    cpu.cp2_ctrl[GTE_RT13RT21] = 0;
    cpu.cp2_ctrl[GTE_RT22RT23] = (0 << 16) | (4096 & 0xFFFF);
    cpu.cp2_ctrl[GTE_RT31RT32] = 0;
    cpu.cp2_ctrl[GTE_RT33]     = 4096;
    cpu.cp2_ctrl[GTE_TRX] = 100;
    cpu.cp2_ctrl[GTE_TRY] = 200;
    cpu.cp2_ctrl[GTE_TRZ] = 300;

    cpu.cp2_data[GTE_VXY0] = PACK_VXY(10, 20);
    cpu.cp2_data[GTE_VZ0]  = 30;

    /* MVMVA: sf=1, lm=0, mx=0(RT), v=0(V0), cv=0(TR) */
    EMIT(GTE_CMD_MVMVA(1, 0, 0, 0, 0));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 110);
    EXPECT_CP2_DATA(GTE_MAC2, 220);
    EXPECT_CP2_DATA(GTE_MAC3, 330);
    EXPECT_CP2_DATA(GTE_IR1, 110);
    EXPECT_CP2_DATA(GTE_IR2, 220);
    EXPECT_CP2_DATA(GTE_IR3, 330);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 23: NCS — Normal Color Single (sf=1, lm=1)
 *
 * Identity Light, identity Color, BK=(0,0,0)
 * V0 = (1000,1000,1000), RGBC code=0x80
 *
 * Step 1: L×V0 → IR=(1000,1000,1000)
 * Step 2: BK + C×IR → IR=(1000,1000,1000)
 * push_color: r=g=b=1000>>4=62, code=0x80
 * RGB2 = 62|(62<<8)|(62<<16)|(0x80<<24) = 0x803E3E3E
 * ================================================================ */
static void test_gte_ncs(void)
{
    BEGIN_TEST("gte_ncs");
    gte_enable_cop2();

    gte_set_identity_light();
    gte_set_identity_color();
    gte_set_zero_bk();

    cpu.cp2_data[GTE_VXY0] = PACK_VXY(1000, 1000);
    cpu.cp2_data[GTE_VZ0]  = 1000;
    cpu.cp2_data[GTE_RGBC] = 0x80000000;  /* code=0x80 */
    cpu.cp2_data[GTE_RGB0] = 0;
    cpu.cp2_data[GTE_RGB1] = 0;
    cpu.cp2_data[GTE_RGB2] = 0;

    EMIT(GTE_CMD_NCS(1, 1));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 1000);
    EXPECT_CP2_DATA(GTE_MAC2, 1000);
    EXPECT_CP2_DATA(GTE_MAC3, 1000);
    EXPECT_CP2_DATA(GTE_IR1, 1000);
    EXPECT_CP2_DATA(GTE_IR2, 1000);
    EXPECT_CP2_DATA(GTE_IR3, 1000);
    EXPECT_CP2_DATA(GTE_RGB2, 0x803E3E3E);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 24: NCCS — Normal Color Color Single (sf=1, lm=1)
 *
 * Same setup as NCS but with RGBC=(128,128,128)
 * Step 1-2: IR=(1000,1000,1000) same as NCS
 * Step 3: MAC = (128*1000)<<4 = 2048000
 * MAC>>12 = 500, IR=500
 * color = 500>>4 = 31
 * RGB2 = 31|(31<<8)|(31<<16) = 0x001F1F1F
 * ================================================================ */
static void test_gte_nccs(void)
{
    BEGIN_TEST("gte_nccs");
    gte_enable_cop2();

    gte_set_identity_light();
    gte_set_identity_color();
    gte_set_zero_bk();

    cpu.cp2_data[GTE_VXY0] = PACK_VXY(1000, 1000);
    cpu.cp2_data[GTE_VZ0]  = 1000;
    cpu.cp2_data[GTE_RGBC] = 0x00808080;  /* R=128, G=128, B=128 */
    cpu.cp2_data[GTE_RGB0] = 0;
    cpu.cp2_data[GTE_RGB1] = 0;
    cpu.cp2_data[GTE_RGB2] = 0;

    EMIT(GTE_CMD_NCCS(1, 1));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 500);
    EXPECT_CP2_DATA(GTE_MAC2, 500);
    EXPECT_CP2_DATA(GTE_MAC3, 500);
    EXPECT_CP2_DATA(GTE_IR1, 500);
    EXPECT_CP2_DATA(GTE_IR2, 500);
    EXPECT_CP2_DATA(GTE_IR3, 500);
    EXPECT_CP2_DATA(GTE_RGB2, 0x001F1F1F);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 25: CC — Color Color (sf=1, lm=1)
 *
 * Identity Color, BK=0, RGBC=(128,128,128)
 * IR1=IR2=IR3=1000 (pre-set lighting result)
 * Step 1: C×IR + BK → IR stays 1000
 * Step 2: MAC = (128*1000)<<4 = 2048000>>12 = 500
 * RGB2 = 0x001F1F1F
 * ================================================================ */
static void test_gte_cc(void)
{
    BEGIN_TEST("gte_cc");
    gte_enable_cop2();

    gte_set_identity_color();
    gte_set_zero_bk();

    cpu.cp2_data[GTE_IR1]  = 1000;
    cpu.cp2_data[GTE_IR2]  = 1000;
    cpu.cp2_data[GTE_IR3]  = 1000;
    cpu.cp2_data[GTE_RGBC] = 0x00808080;
    cpu.cp2_data[GTE_RGB0] = 0;
    cpu.cp2_data[GTE_RGB1] = 0;
    cpu.cp2_data[GTE_RGB2] = 0;

    EMIT(GTE_CMD_CC(1, 1));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 500);
    EXPECT_CP2_DATA(GTE_MAC2, 500);
    EXPECT_CP2_DATA(GTE_MAC3, 500);
    EXPECT_CP2_DATA(GTE_IR1, 500);
    EXPECT_CP2_DATA(GTE_IR2, 500);
    EXPECT_CP2_DATA(GTE_IR3, 500);
    EXPECT_CP2_DATA(GTE_RGB2, 0x001F1F1F);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 26: NCDS — Normal Color Depth Cue Single (sf=1, lm=1)
 *
 * Identity Light, identity Color, BK=0
 * V0=(1000,1000,1000), RGBC=(128,128,128), IR0=2048, FC=(200,200,200)
 *
 * Step 1: L×V0 → IR=(1000,1000,1000)
 * Step 2: C×IR+BK → IR=(1000,1000,1000)
 * Step 3: acc = (128*1000)<<4 = 2048000; MAC>>12=500 (intermediate)
 *         But actually step 3 is:
 *         acc = (RGBC.ch × IR_ch) << 4 → same 2048000
 * Step 4: interpolate toward FC:
 *         fc=200<<12=819200, d=819200-2048000=-1228800
 *         tmp_ir=-1228800>>12=-300 (clamped, but with lm=0 for intermediate)
 *         result = -300*2048+2048000=1433600
 *         MAC = 1433600>>12=350, IR=350
 * color = 350>>4=21
 * RGB2 = 21|(21<<8)|(21<<16) = 0x00151515
 * ================================================================ */
static void test_gte_ncds(void)
{
    BEGIN_TEST("gte_ncds");
    gte_enable_cop2();

    gte_set_identity_light();
    gte_set_identity_color();
    gte_set_zero_bk();

    cpu.cp2_data[GTE_VXY0] = PACK_VXY(1000, 1000);
    cpu.cp2_data[GTE_VZ0]  = 1000;
    cpu.cp2_data[GTE_RGBC] = 0x00808080;
    cpu.cp2_data[GTE_IR0]  = 0x800;  /* 2048 */
    cpu.cp2_ctrl[GTE_RFC]  = 200;
    cpu.cp2_ctrl[GTE_GFC]  = 200;
    cpu.cp2_ctrl[GTE_BFC]  = 200;
    cpu.cp2_data[GTE_RGB0] = 0;
    cpu.cp2_data[GTE_RGB1] = 0;
    cpu.cp2_data[GTE_RGB2] = 0;

    EMIT(GTE_CMD_NCDS(1, 1));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 350);
    EXPECT_CP2_DATA(GTE_MAC2, 350);
    EXPECT_CP2_DATA(GTE_MAC3, 350);
    EXPECT_CP2_DATA(GTE_IR1, 350);
    EXPECT_CP2_DATA(GTE_IR2, 350);
    EXPECT_CP2_DATA(GTE_IR3, 350);
    EXPECT_CP2_DATA(GTE_RGB2, 0x00151515);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 27: CDP — Color Depth Cue (sf=1, lm=1)
 *
 * Like CC + depth cue interpolation.
 * Identity Color, BK=0, RGBC=(128,128,128)
 * IR1=IR2=IR3=1000, IR0=2048, FC=(200,200,200)
 *
 * Step 1: C×IR + BK → IR=1000
 * Step 2: acc=(128*1000)<<4=2048000 → MAC>>12=500
 * Step 3: interpolate: fc=819200, d=819200-2048000=-1228800
 *         tmp=-300, result=-300*2048+2048000=1433600
 *         MAC=350, IR=350
 * RGB2 = 0x00151515
 * ================================================================ */
static void test_gte_cdp(void)
{
    BEGIN_TEST("gte_cdp");
    gte_enable_cop2();

    gte_set_identity_color();
    gte_set_zero_bk();

    cpu.cp2_data[GTE_IR1]  = 1000;
    cpu.cp2_data[GTE_IR2]  = 1000;
    cpu.cp2_data[GTE_IR3]  = 1000;
    cpu.cp2_data[GTE_IR0]  = 0x800;
    cpu.cp2_data[GTE_RGBC] = 0x00808080;
    cpu.cp2_ctrl[GTE_RFC]  = 200;
    cpu.cp2_ctrl[GTE_GFC]  = 200;
    cpu.cp2_ctrl[GTE_BFC]  = 200;
    cpu.cp2_data[GTE_RGB0] = 0;
    cpu.cp2_data[GTE_RGB1] = 0;
    cpu.cp2_data[GTE_RGB2] = 0;

    EMIT(GTE_CMD_CDP(1, 1));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 350);
    EXPECT_CP2_DATA(GTE_MAC2, 350);
    EXPECT_CP2_DATA(GTE_MAC3, 350);
    EXPECT_CP2_DATA(GTE_IR1, 350);
    EXPECT_CP2_DATA(GTE_IR2, 350);
    EXPECT_CP2_DATA(GTE_IR3, 350);
    EXPECT_CP2_DATA(GTE_RGB2, 0x00151515);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 28: NCT — Normal Color Triple (sf=1, lm=1)
 *
 * Same as NCS but for 3 vertices.  Uses V0, V1, V2.
 * All set to (1000,1000,1000). Final result from V2.
 * Expected: same as NCS → RGB2 = 0x803E3E3E
 * ================================================================ */
static void test_gte_nct(void)
{
    BEGIN_TEST("gte_nct");
    gte_enable_cop2();

    gte_set_identity_light();
    gte_set_identity_color();
    gte_set_zero_bk();

    cpu.cp2_data[GTE_VXY0] = PACK_VXY(1000, 1000);
    cpu.cp2_data[GTE_VZ0]  = 1000;
    cpu.cp2_data[GTE_VXY1] = PACK_VXY(1000, 1000);
    cpu.cp2_data[GTE_VZ1]  = 1000;
    cpu.cp2_data[GTE_VXY2] = PACK_VXY(1000, 1000);
    cpu.cp2_data[GTE_VZ2]  = 1000;
    cpu.cp2_data[GTE_RGBC] = 0x80000000;
    cpu.cp2_data[GTE_RGB0] = 0;
    cpu.cp2_data[GTE_RGB1] = 0;
    cpu.cp2_data[GTE_RGB2] = 0;

    EMIT(GTE_CMD_NCT(1, 1));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 1000);
    EXPECT_CP2_DATA(GTE_MAC2, 1000);
    EXPECT_CP2_DATA(GTE_MAC3, 1000);
    /* RGB FIFO should have 3 entries from 3 iterations */
    EXPECT_CP2_DATA(GTE_RGB2, 0x803E3E3E);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 29: NCCT — Normal Color Color Triple (sf=1, lm=1)
 *
 * Same as NCCS but for 3 vertices. All same → RGB2 = 0x001F1F1F
 * ================================================================ */
static void test_gte_ncct(void)
{
    BEGIN_TEST("gte_ncct");
    gte_enable_cop2();

    gte_set_identity_light();
    gte_set_identity_color();
    gte_set_zero_bk();

    cpu.cp2_data[GTE_VXY0] = PACK_VXY(1000, 1000);
    cpu.cp2_data[GTE_VZ0]  = 1000;
    cpu.cp2_data[GTE_VXY1] = PACK_VXY(1000, 1000);
    cpu.cp2_data[GTE_VZ1]  = 1000;
    cpu.cp2_data[GTE_VXY2] = PACK_VXY(1000, 1000);
    cpu.cp2_data[GTE_VZ2]  = 1000;
    cpu.cp2_data[GTE_RGBC] = 0x00808080;
    cpu.cp2_data[GTE_RGB0] = 0;
    cpu.cp2_data[GTE_RGB1] = 0;
    cpu.cp2_data[GTE_RGB2] = 0;

    EMIT(GTE_CMD_NCCT(1, 1));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 500);
    EXPECT_CP2_DATA(GTE_MAC2, 500);
    EXPECT_CP2_DATA(GTE_MAC3, 500);
    EXPECT_CP2_DATA(GTE_RGB2, 0x001F1F1F);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 30: NCDT — Normal Color Depth Cue Triple (sf=1, lm=1)
 *
 * Same as NCDS but for 3 vertices.
 * Expected: same as NCDS → MAC=350, RGB2=0x00151515
 * ================================================================ */
static void test_gte_ncdt(void)
{
    BEGIN_TEST("gte_ncdt");
    gte_enable_cop2();

    gte_set_identity_light();
    gte_set_identity_color();
    gte_set_zero_bk();

    cpu.cp2_data[GTE_VXY0] = PACK_VXY(1000, 1000);
    cpu.cp2_data[GTE_VZ0]  = 1000;
    cpu.cp2_data[GTE_VXY1] = PACK_VXY(1000, 1000);
    cpu.cp2_data[GTE_VZ1]  = 1000;
    cpu.cp2_data[GTE_VXY2] = PACK_VXY(1000, 1000);
    cpu.cp2_data[GTE_VZ2]  = 1000;
    cpu.cp2_data[GTE_RGBC] = 0x00808080;
    cpu.cp2_data[GTE_IR0]  = 0x800;
    cpu.cp2_ctrl[GTE_RFC]  = 200;
    cpu.cp2_ctrl[GTE_GFC]  = 200;
    cpu.cp2_ctrl[GTE_BFC]  = 200;
    cpu.cp2_data[GTE_RGB0] = 0;
    cpu.cp2_data[GTE_RGB1] = 0;
    cpu.cp2_data[GTE_RGB2] = 0;

    EMIT(GTE_CMD_NCDT(1, 1));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC1, 350);
    EXPECT_CP2_DATA(GTE_MAC2, 350);
    EXPECT_CP2_DATA(GTE_MAC3, 350);
    EXPECT_CP2_DATA(GTE_RGB2, 0x00151515);
    EXPECT_CP2_CTRL(GTE_FLAG_CTRL, 0);
    END_TEST();
}


/* ================================================================
 * Test 31: RTPS — Perspective Transform Single (sf=1, lm=1)
 *
 * Identity RT, TR=(0,0,200), V0=(0,0,0)
 * OFX=160<<16, OFY=120<<16, H=100, DQA=0, DQB=0
 * MAC1=0, MAC2=0, MAC3=200
 * SZ3=200
 * SX2 = (div × 0 + OFX)>>16 = 160
 * SY2 = (div × 0 + OFY)>>16 = 120
 * Screen center for vertex at camera center
 * ================================================================ */
static void test_gte_rtps_center(void)
{
    BEGIN_TEST("gte_rtps_center");
    gte_enable_cop2();
    gte_set_identity();  /* identity RT, TR=(0,0,0) */

    /* Override TRZ for depth */
    cpu.cp2_ctrl[GTE_TRZ] = 200;
    cpu.cp2_ctrl[GTE_OFX] = 160 << 16;
    cpu.cp2_ctrl[GTE_OFY] = 120 << 16;
    cpu.cp2_ctrl[GTE_H]   = 100;
    cpu.cp2_ctrl[GTE_DQA] = 0;
    cpu.cp2_ctrl[GTE_DQB] = 0;

    cpu.cp2_data[GTE_VXY0] = PACK_VXY(0, 0);
    cpu.cp2_data[GTE_VZ0]  = 0;

    EMIT(GTE_CMD_RTPS(1, 1));

    RUN(200);

    EXPECT_CP2_DATA(GTE_MAC3, 200);
    EXPECT_CP2_DATA(GTE_SZ3, 200);
    /* Screen center should be (160, 120) */
    EXPECT_CP2_DATA(GTE_SXY2, PACK_SXY(160, 120));
    END_TEST();
}


/* ================================================================
 * Test 32: RTPT — Perspective Transform Triple (sf=1, lm=1)
 *
 * Same setup as RTPS but tests all 3 vertices.
 * V0=V1=V2=(0,0,0), TRZ=200.  All should project to screen center.
 * ================================================================ */
static void test_gte_rtpt(void)
{
    BEGIN_TEST("gte_rtpt");
    gte_enable_cop2();
    gte_set_identity();

    cpu.cp2_ctrl[GTE_TRZ] = 200;
    cpu.cp2_ctrl[GTE_OFX] = 160 << 16;
    cpu.cp2_ctrl[GTE_OFY] = 120 << 16;
    cpu.cp2_ctrl[GTE_H]   = 100;
    cpu.cp2_ctrl[GTE_DQA] = 0;
    cpu.cp2_ctrl[GTE_DQB] = 0;

    cpu.cp2_data[GTE_VXY0] = 0;
    cpu.cp2_data[GTE_VZ0]  = 0;
    cpu.cp2_data[GTE_VXY1] = 0;
    cpu.cp2_data[GTE_VZ1]  = 0;
    cpu.cp2_data[GTE_VXY2] = 0;
    cpu.cp2_data[GTE_VZ2]  = 0;

    EMIT(GTE_CMD_RTPT(1, 1));

    RUN(300);

    /* Final SXY2 from V2 should be screen center */
    EXPECT_CP2_DATA(GTE_SXY2, PACK_SXY(160, 120));
    EXPECT_CP2_DATA(GTE_SZ3, 200);
    END_TEST();
}


/* ================================================================
 *  Category runner
 * ================================================================ */
void pg_run_gte_tests(void)
{
    printf("--- GTE Tests ---\n");
    /* Original dirty-tracking / COP2 tests (1-5) */
    test_gte_mtc2_mfc2_roundtrip();
    test_gte_preserves_slots();
    test_gte_mfc2_then_sw();
    test_gte_multi_call();
    test_gte_cu2_dirty_mask_regression();
    /* NCLIP tests (6-10) */
    test_gte_nclip_positive();
    test_gte_nclip_negative();
    test_gte_nclip_zero();
    test_gte_nclip_negcoords();
    test_gte_nclip_mfc2_readback();
    /* Z average (11-12) */
    test_gte_avsz3();
    test_gte_avsz4();
    /* Simple vector ops (13-15) */
    test_gte_op();
    test_gte_sqr_sf0();
    test_gte_sqr_sf1();
    /* Interpolation (16-17) */
    test_gte_gpf();
    test_gte_gpl();
    /* Depth cueing (18-21) */
    test_gte_dpcs();
    test_gte_intpl();
    test_gte_dcpl();
    test_gte_dpct();
    /* Matrix ops (22) */
    test_gte_mvmva();
    /* Normal color (23-30) */
    test_gte_ncs();
    test_gte_nccs();
    test_gte_cc();
    test_gte_ncds();
    test_gte_cdp();
    test_gte_nct();
    test_gte_ncct();
    test_gte_ncdt();
    /* Perspective transform (31-32) */
    test_gte_rtps_center();
    test_gte_rtpt();
}
