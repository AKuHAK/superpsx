/*
 * JIT Playground — Test Cases
 *
 * 34 micro-tests covering ALU, shifts, mul/div, comparisons,
 * loads/stores, branches, and instruction interactions.
 *
 * Each test:
 *   1. Sets initial CPU/memory state
 *   2. Emits R3000A opcodes into PSX RAM
 *   3. Compiles + executes via the real JIT
 *   4. Asserts expected CPU/memory state
 */
#include "playground.h"

/* ================================================================
 *  ALU Basic (tests 1-8)
 * ================================================================ */

static void test_addu_basic(void)
{
    BEGIN_TEST("addu_basic");
    SET_REG(R_V0, 100);
    SET_REG(R_V1, 200);
    EMIT(PSX_ADDU(R_A0, R_V0, R_V1));   /* a0 = v0 + v1 = 300 */
    RUN(1000);
    EXPECT_REG(R_A0, 300);
    END_TEST();
}

static void test_addu_overflow_wrap(void)
{
    BEGIN_TEST("addu_overflow_wrap");
    SET_REG(R_V0, 0xFFFFFFFF);
    SET_REG(R_V1, 1);
    EMIT(PSX_ADDU(R_A0, R_V0, R_V1));   /* a0 = 0 (wraps) */
    RUN(1000);
    EXPECT_REG(R_A0, 0);
    END_TEST();
}

static void test_subu_basic(void)
{
    BEGIN_TEST("subu_basic");
    SET_REG(R_V0, 500);
    SET_REG(R_V1, 200);
    EMIT(PSX_SUBU(R_A0, R_V0, R_V1));   /* a0 = 300 */
    RUN(1000);
    EXPECT_REG(R_A0, 300);
    END_TEST();
}

static void test_and_or_xor_nor(void)
{
    BEGIN_TEST("and_or_xor_nor");
    SET_REG(R_V0, 0xFF00FF00);
    SET_REG(R_V1, 0x0F0F0F0F);
    EMIT(PSX_AND(R_A0, R_V0, R_V1));   /* a0 = 0x0F000F00 */
    EMIT(PSX_OR(R_A1, R_V0, R_V1));    /* a1 = 0xFF0FFF0F */
    EMIT(PSX_XOR(R_A2, R_V0, R_V1));   /* a2 = 0xF00FF00F */
    EMIT(PSX_NOR(R_A3, R_V0, R_V1));   /* a3 = ~(v0|v1) = 0x00F000F0 */
    RUN(1000);
    EXPECT_REG(R_A0, 0x0F000F00);
    EXPECT_REG(R_A1, 0xFF0FFF0F);
    EXPECT_REG(R_A2, 0xF00FF00F);
    EXPECT_REG(R_A3, 0x00F000F0);
    END_TEST();
}

static void test_addiu_signext(void)
{
    BEGIN_TEST("addiu_signext");
    SET_REG(R_V0, 100);
    EMIT(PSX_ADDIU(R_A0, R_V0, (uint16_t)(-10)));  /* a0 = 100 + (-10) = 90 */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 42));              /* a1 = 0 + 42 = 42 */
    RUN(1000);
    EXPECT_REG(R_A0, 90);
    EXPECT_REG(R_A1, 42);
    END_TEST();
}

static void test_lui_ori(void)
{
    BEGIN_TEST("lui_ori");
    EMIT(PSX_LUI(R_V0, 0x1234));          /* v0 = 0x12340000 */
    EMIT(PSX_ORI(R_V0, R_V0, 0x5678));    /* v0 = 0x12345678 */
    RUN(1000);
    EXPECT_REG(R_V0, 0x12345678);
    END_TEST();
}

static void test_andi_xori(void)
{
    BEGIN_TEST("andi_xori");
    SET_REG(R_V0, 0xDEADBEEF);
    EMIT(PSX_ANDI(R_A0, R_V0, 0x00FF));   /* a0 = 0xEF */
    EMIT(PSX_XORI(R_A1, R_V0, 0xFFFF));   /* a1 = 0xDEAD4110 */
    RUN(1000);
    EXPECT_REG(R_A0, 0x000000EF);
    EXPECT_REG(R_A1, 0xDEAD4110);
    END_TEST();
}

static void test_addu_zero_reg(void)
{
    /* $zero must always read as 0 and writes must be ignored */
    BEGIN_TEST("zero_register");
    SET_REG(R_V0, 42);
    EMIT(PSX_ADDU(R_ZERO, R_V0, R_V0));   /* $zero = v0 + v0 → must stay 0 */
    EMIT(PSX_ADDU(R_A0, R_ZERO, R_ZERO)); /* a0 = 0 + 0 = 0 */
    RUN(1000);
    EXPECT_REG(R_ZERO, 0);
    EXPECT_REG(R_A0, 0);
    END_TEST();
}

/* ================================================================
 *  Shifts (tests 9-12)
 * ================================================================ */

static void test_sll_srl_sra(void)
{
    BEGIN_TEST("sll_srl_sra");
    SET_REG(R_V0, 0x00000001);
    EMIT(PSX_SLL(R_A0, R_V0, 8));         /* a0 = 0x100 */
    SET_REG(R_V1, 0x00008000);
    EMIT(PSX_SRL(R_A1, R_V1, 4));         /* a1 = 0x800 */
    SET_REG(R_T0, 0x80000000);
    EMIT(PSX_SRA(R_A2, R_T0, 4));         /* a2 = 0xF8000000 (arithmetic) */
    RUN(1000);
    EXPECT_REG(R_A0, 0x00000100);
    EXPECT_REG(R_A1, 0x00000800);
    EXPECT_REG(R_A2, 0xF8000000);
    END_TEST();
}

static void test_sllv_srlv_srav(void)
{
    BEGIN_TEST("sllv_srlv_srav");
    SET_REG(R_V0, 0x00000001);
    SET_REG(R_V1, 16);
    EMIT(PSX_SLLV(R_A0, R_V0, R_V1));    /* a0 = 1 << 16 = 0x10000 */
    SET_REG(R_T0, 0x00FF0000);
    SET_REG(R_T1, 8);
    EMIT(PSX_SRLV(R_A1, R_T0, R_T1));    /* a1 = 0x00FF0000 >> 8 = 0x0000FF00 */
    SET_REG(R_T2, 0x80000000);
    SET_REG(R_T3, 31);
    EMIT(PSX_SRAV(R_A2, R_T2, R_T3));    /* a2 = 0xFFFFFFFF (sign-extended) */
    RUN(1000);
    EXPECT_REG(R_A0, 0x00010000);
    EXPECT_REG(R_A1, 0x0000FF00);
    EXPECT_REG(R_A2, 0xFFFFFFFF);
    END_TEST();
}

static void test_shift_by_zero(void)
{
    BEGIN_TEST("shift_by_zero");
    SET_REG(R_V0, 0xDEADBEEF);
    EMIT(PSX_SLL(R_A0, R_V0, 0));         /* a0 = v0 (NOP-like) */
    EMIT(PSX_SRL(R_A1, R_V0, 0));         /* a1 = v0 */
    EMIT(PSX_SRA(R_A2, R_V0, 0));         /* a2 = v0 */
    RUN(1000);
    EXPECT_REG(R_A0, 0xDEADBEEF);
    EXPECT_REG(R_A1, 0xDEADBEEF);
    EXPECT_REG(R_A2, 0xDEADBEEF);
    END_TEST();
}

static void test_sra_sign_extend(void)
{
    BEGIN_TEST("sra_sign_extend");
    /* Positive value: SRA should zero-fill MSBs */
    SET_REG(R_V0, 0x7FFFFFFF);
    EMIT(PSX_SRA(R_A0, R_V0, 16));        /* a0 = 0x00007FFF */
    /* Negative value: SRA should one-fill MSBs */
    SET_REG(R_V1, 0x80000000);
    EMIT(PSX_SRA(R_A1, R_V1, 16));        /* a1 = 0xFFFF8000 */
    RUN(1000);
    EXPECT_REG(R_A0, 0x00007FFF);
    EXPECT_REG(R_A1, 0xFFFF8000);
    END_TEST();
}

/* ================================================================
 *  Multiply / Divide (tests 13-16)
 * ================================================================ */

static void test_mult_basic(void)
{
    BEGIN_TEST("mult_basic");
    SET_REG(R_V0, 100);
    SET_REG(R_V1, 200);
    EMIT(PSX_MULT(R_V0, R_V1));           /* HI:LO = 100 * 200 = 20000 */
    EMIT(PSX_MFLO(R_A0));                 /* a0 = 20000 */
    EMIT(PSX_MFHI(R_A1));                 /* a1 = 0 */
    RUN(2000);
    EXPECT_REG(R_A0, 20000);
    EXPECT_REG(R_A1, 0);
    END_TEST();
}

static void test_multu_basic(void)
{
    BEGIN_TEST("multu_basic");
    SET_REG(R_V0, 0x10000);
    SET_REG(R_V1, 0x10000);
    EMIT(PSX_MULTU(R_V0, R_V1));          /* 64K * 64K = 4G = 0x100000000 */
    EMIT(PSX_MFLO(R_A0));                 /* a0 = 0x00000000 */
    EMIT(PSX_MFHI(R_A1));                 /* a1 = 0x00000001 */
    RUN(2000);
    EXPECT_REG(R_A0, 0x00000000);
    EXPECT_REG(R_A1, 0x00000001);
    END_TEST();
}

static void test_div_basic(void)
{
    BEGIN_TEST("div_basic");
    SET_REG(R_V0, 100);
    SET_REG(R_V1, 7);
    EMIT(PSX_DIV(R_V0, R_V1));            /* 100/7 = 14 rem 2 */
    EMIT(PSX_MFLO(R_A0));                 /* a0 = 14 (quotient) */
    EMIT(PSX_MFHI(R_A1));                 /* a1 = 2  (remainder) */
    RUN(5000);
    EXPECT_REG(R_A0, 14);
    EXPECT_REG(R_A1, 2);
    END_TEST();
}

static void test_divu_basic(void)
{
    BEGIN_TEST("divu_basic");
    SET_REG(R_V0, 0xFFFFFFFF);            /* 4294967295 */
    SET_REG(R_V1, 10);
    EMIT(PSX_DIVU(R_V0, R_V1));           /* 4294967295 / 10 = 429496729 rem 5 */
    EMIT(PSX_MFLO(R_A0));
    EMIT(PSX_MFHI(R_A1));
    RUN(5000);
    EXPECT_REG(R_A0, 429496729u);
    EXPECT_REG(R_A1, 5);
    END_TEST();
}

/* ================================================================
 *  Comparisons (tests 17-19)
 * ================================================================ */

static void test_slt_signed(void)
{
    BEGIN_TEST("slt_signed");
    SET_REG(R_V0, (uint32_t)(-5));        /* v0 = -5 (0xFFFFFFFB) */
    SET_REG(R_V1, 5);
    EMIT(PSX_SLT(R_A0, R_V0, R_V1));     /* a0 = (-5 < 5) = 1 */
    EMIT(PSX_SLT(R_A1, R_V1, R_V0));     /* a1 = (5 < -5) = 0 */
    EMIT(PSX_SLT(R_A2, R_V0, R_V0));     /* a2 = (-5 < -5) = 0 */
    RUN(1000);
    EXPECT_REG(R_A0, 1);
    EXPECT_REG(R_A1, 0);
    EXPECT_REG(R_A2, 0);
    END_TEST();
}

static void test_sltu_unsigned(void)
{
    BEGIN_TEST("sltu_unsigned");
    SET_REG(R_V0, 0xFFFFFFFB);            /* Large unsigned */
    SET_REG(R_V1, 5);
    EMIT(PSX_SLTU(R_A0, R_V1, R_V0));    /* a0 = (5 < 0xFFFFFFFB) = 1 */
    EMIT(PSX_SLTU(R_A1, R_V0, R_V1));    /* a1 = (0xFFFFFFFB < 5) = 0 */
    RUN(1000);
    EXPECT_REG(R_A0, 1);
    EXPECT_REG(R_A1, 0);
    END_TEST();
}

static void test_slti_sltiu(void)
{
    BEGIN_TEST("slti_sltiu");
    SET_REG(R_V0, 10);
    EMIT(PSX_SLTI(R_A0, R_V0, 20));      /* a0 = (10 < 20) = 1 */
    EMIT(PSX_SLTI(R_A1, R_V0, 5));       /* a1 = (10 < 5) = 0 */
    SET_REG(R_V1, 0xFFFFFFFB);            /* -5 as signed */
    EMIT(PSX_SLTIU(R_A2, R_V1, 0));      /* a2 = (0xFFFFFFFB < 0) unsigned. imm sign-extends to 0x00000000 → 0 */
    RUN(1000);
    EXPECT_REG(R_A0, 1);
    EXPECT_REG(R_A1, 0);
    EXPECT_REG(R_A2, 0);
    END_TEST();
}

/* ================================================================
 *  Load / Store (tests 20-25)
 * ================================================================ */

static void test_lw_sw_basic(void)
{
    BEGIN_TEST("lw_sw_basic");
    /* Use data area at 0x80020000 = offset 0x20000.
     * We put base in a register via LUI+ORI. */
    EMIT(PSX_LUI(R_T0, 0x8002));           /* t0 = 0x80020000 */
    SET_REG(R_V0, 0xCAFEBABE);
    EMIT(PSX_SW(R_V0, 0, R_T0));           /* MEM[0x80020000] = 0xCAFEBABE */
    EMIT(PSX_LW(R_A0, 0, R_T0));           /* a0 = MEM[0x80020000] */
    RUN(2000);
    EXPECT_REG(R_A0, 0xCAFEBABE);
    EXPECT_MEM32(PG_DATA_OFFSET, 0xCAFEBABE);
    END_TEST();
}

static void test_lb_sb_signext(void)
{
    BEGIN_TEST("lb_sb_signext");
    EMIT(PSX_LUI(R_T0, 0x8002));           /* t0 = 0x80020000 */
    SET_REG(R_V0, 0xFF);                    /* -1 as byte */
    EMIT(PSX_SB(R_V0, 0, R_T0));           /* MEM[off] = 0xFF */
    EMIT(PSX_LB(R_A0, 0, R_T0));           /* a0 = sign-extend(0xFF) = 0xFFFFFFFF */
    EMIT(PSX_LBU(R_A1, 0, R_T0));          /* a1 = zero-extend(0xFF) = 0x000000FF */
    RUN(2000);
    EXPECT_REG(R_A0, 0xFFFFFFFF);
    EXPECT_REG(R_A1, 0x000000FF);
    END_TEST();
}

static void test_lh_sh(void)
{
    BEGIN_TEST("lh_sh");
    EMIT(PSX_LUI(R_T0, 0x8002));
    SET_REG(R_V0, 0x8001);                 /* negative halfword */
    EMIT(PSX_SH(R_V0, 0, R_T0));
    EMIT(PSX_LH(R_A0, 0, R_T0));           /* sign-ext: 0xFFFF8001 */
    EMIT(PSX_LHU(R_A1, 0, R_T0));          /* zero-ext: 0x00008001 */
    RUN(2000);
    EXPECT_REG(R_A0, 0xFFFF8001);
    EXPECT_REG(R_A1, 0x00008001);
    END_TEST();
}

static void test_sw_lw_offset(void)
{
    BEGIN_TEST("sw_lw_offset");
    EMIT(PSX_LUI(R_T0, 0x8002));           /* base = 0x80020000 */
    SET_REG(R_V0, 0x11111111);
    SET_REG(R_V1, 0x22222222);
    EMIT(PSX_SW(R_V0, 0, R_T0));           /* [base+0] = 0x11111111 */
    EMIT(PSX_SW(R_V1, 4, R_T0));           /* [base+4] = 0x22222222 */
    EMIT(PSX_LW(R_A0, 0, R_T0));           /* a0 = 0x11111111 */
    EMIT(PSX_LW(R_A1, 4, R_T0));           /* a1 = 0x22222222 */
    RUN(2000);
    EXPECT_REG(R_A0, 0x11111111);
    EXPECT_REG(R_A1, 0x22222222);
    END_TEST();
}

static void test_lwl_lwr(void)
{
    BEGIN_TEST("lwl_lwr");
    /* Store a known pattern at data area */
    SET_MEM32(PG_DATA_OFFSET, 0xAABBCCDD);
    EMIT(PSX_LUI(R_T0, 0x8002));           /* base = 0x80020000 */
    SET_REG(R_A0, 0);
    /* LWL loads the high bytes, LWR loads the low bytes.
     * At aligned address, LWL rt,3(base) + LWR rt,0(base) = full word. */
    EMIT(PSX_LWL(R_A0, 3, R_T0));
    EMIT(PSX_LWR(R_A0, 0, R_T0));
    RUN(2000);
    EXPECT_REG(R_A0, 0xAABBCCDD);
    END_TEST();
}

static void test_swl_swr(void)
{
    BEGIN_TEST("swl_swr");
    /* Clear data area */
    SET_MEM32(PG_DATA_OFFSET, 0);
    EMIT(PSX_LUI(R_T0, 0x8002));           /* base = 0x80020000 */
    SET_REG(R_V0, 0x12345678);
    /* SWL + SWR at aligned address = full word store */
    EMIT(PSX_SWL(R_V0, 3, R_T0));
    EMIT(PSX_SWR(R_V0, 0, R_T0));
    EMIT(PSX_LW(R_A0, 0, R_T0));
    RUN(2000);
    EXPECT_REG(R_A0, 0x12345678);
    EXPECT_MEM32(PG_DATA_OFFSET, 0x12345678);
    END_TEST();
}

/* ================================================================
 *  Branches (tests 26-30)
 * ================================================================ */

static void test_beq_taken(void)
{
    BEGIN_TEST("beq_taken");
    SET_REG(R_V0, 42);
    SET_REG(R_V1, 42);
    /*
     * 0: BEQ v0, v1, +2     (skip next insn, jump to insn at offset 3)
     * 1: NOP                 (delay slot)
     * 2: ADDIU a0, zero, 99  (skipped)
     * 3: ADDIU a0, zero, 77  (branch target)
     */
    EMIT(PSX_BEQ(R_V0, R_V1, 2));         /* branch to PC+4+2*4 = insn 3 */
    EMIT(PSX_NOP());                       /* delay slot */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 99));    /* skipped */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 77));    /* branch target */
    RUN(2000);
    EXPECT_REG(R_A0, 77);
    END_TEST();
}

static void test_beq_not_taken(void)
{
    BEGIN_TEST("beq_not_taken");
    SET_REG(R_V0, 1);
    SET_REG(R_V1, 2);
    /*
     * 0: BEQ v0, v1, +2     (not taken)
     * 1: NOP                 (delay slot — executes either way)
     * 2: ADDIU a0, zero, 99  (fall-through)
     */
    EMIT(PSX_BEQ(R_V0, R_V1, 2));
    EMIT(PSX_NOP());
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 99));
    RUN(2000);
    EXPECT_REG(R_A0, 99);
    END_TEST();
}

static void test_bne_taken(void)
{
    BEGIN_TEST("bne_taken");
    SET_REG(R_V0, 1);
    SET_REG(R_V1, 2);
    EMIT(PSX_BNE(R_V0, R_V1, 2));         /* 1 != 2 → taken */
    EMIT(PSX_NOP());
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 99));    /* skipped */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 55));    /* target */
    RUN(2000);
    EXPECT_REG(R_A0, 55);
    END_TEST();
}

static void test_branch_delay_slot(void)
{
    /* Instruction in the delay slot MUST execute regardless of branch outcome */
    BEGIN_TEST("branch_delay_slot");
    SET_REG(R_V0, 1);
    SET_REG(R_V1, 1);
    EMIT(PSX_BEQ(R_V0, R_V1, 2));         /* taken */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 42));    /* delay slot — must execute */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 99));    /* skipped */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 77));    /* branch target */
    RUN(2000);
    EXPECT_REG(R_A0, 42);                 /* delay slot executed */
    EXPECT_REG(R_A1, 77);                 /* branch target executed */
    END_TEST();
}

static void test_branch_delay_store(void)
{
    /* Store in delay slot must complete before branch executes */
    BEGIN_TEST("branch_delay_store");
    EMIT(PSX_LUI(R_T0, 0x8002));           /* t0 = data base */
    SET_REG(R_V0, 0);
    SET_REG(R_V1, 0);
    SET_REG(R_A0, 0xBEEF);
    EMIT(PSX_BEQ(R_V0, R_V1, 2));          /* taken */
    EMIT(PSX_SW(R_A0, 0, R_T0));           /* delay slot: store 0xBEEF */
    EMIT(PSX_NOP());                        /* skipped */
    EMIT(PSX_LW(R_A1, 0, R_T0));           /* target: load it back */
    RUN(2000);
    EXPECT_REG(R_A1, 0xBEEF);
    EXPECT_MEM32(PG_DATA_OFFSET, 0xBEEF);
    END_TEST();
}

/* ================================================================
 *  Instruction Interactions (tests 31-34)
 * ================================================================ */

static void test_store_load_forwarding(void)
{
    /* SW followed by LW to same address */
    BEGIN_TEST("store_load_forward");
    EMIT(PSX_LUI(R_T0, 0x8002));
    SET_REG(R_V0, 0xDEADC0DE);
    EMIT(PSX_SW(R_V0, 0, R_T0));
    EMIT(PSX_LW(R_A0, 0, R_T0));
    RUN(2000);
    EXPECT_REG(R_A0, 0xDEADC0DE);
    END_TEST();
}

static void test_loop_counter(void)
{
    /* Simple loop: decrement counter until zero.
     * Tests BNE + ADDIU interaction, super-block fall-through. */
    BEGIN_TEST("loop_counter");
    SET_REG(R_V0, 5);                      /* counter = 5 */
    SET_REG(R_A0, 0);                      /* accumulator */

    /*
     * loop:
     *   0: ADDIU  a0, a0, 1                (accumulate)
     *   1: ADDIU  v0, v0, -1               (counter--)
     *   2: BNE    v0, zero, -3             (branch to insn 0 if v0 != 0)
     *   3: NOP                              (delay slot)
     *   (fall-through when v0==0)
     */
    EMIT(PSX_ADDIU(R_A0, R_A0, 1));
    EMIT(PSX_ADDIU(R_V0, R_V0, (uint16_t)(-1)));
    EMIT(PSX_BNE(R_V0, R_ZERO, (uint16_t)(-3)));  /* branch offset = -3 → back to insn 0 */
    EMIT(PSX_NOP());

    RUN(50000);  /* enough cycles for 5 iterations */
    EXPECT_REG(R_A0, 5);                   /* accumulated 5 times */
    EXPECT_REG(R_V0, 0);                   /* counter reached 0 */
    END_TEST();
}

static void test_jal_jr_ra(void)
{
    /* JAL writes PC+8 into $ra, then we JR $ra from the subroutine.
     *
     * Layout (all relative to PG_CODE_BASE = 0x80010000):
     *   0: JAL  0x80010014  (= insn 5)     → $ra = 0x80010008
     *   1: NOP               (delay slot)
     *   2: ADDIU a0, zero, 1 (return here) ← $ra points here (0x80010008)
     *
     * At insn 5 (subroutine):
     *   5: ADDIU a1, zero, 2
     *   6: JR    $ra
     *   7: NOP
     */
    BEGIN_TEST("jal_jr_ra");
    uint32_t sub_addr = PG_CODE_BASE + 5 * 4;  /* 0x80010014 */
    uint32_t jal_target = (sub_addr >> 2) & 0x03FFFFFF;
    EMIT(PSX_JAL(jal_target));              /* 0: JAL subroutine */
    EMIT(PSX_NOP());                        /* 1: delay slot */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 1));      /* 2: executed after return */
    /* JR $ra + NOP will be auto-appended by RUN() at insns 3,4 */
    /* Manually place subroutine at insns 5,6,7 */
    pg_ctx.code[5] = PSX_ADDIU(R_A1, R_ZERO, 2);
    pg_ctx.code[6] = PSX_JR(R_RA);
    pg_ctx.code[7] = PSX_NOP();
    pg_ctx.count = 3; /* RUN will append JR+NOP at 3,4 */
    RUN(5000);
    EXPECT_REG(R_A0, 1);                   /* executed after return */
    EXPECT_REG(R_A1, 2);                   /* executed in subroutine */
    EXPECT_REG(R_RA, PG_CODE_BASE + 8);    /* ra = PC+8 of JAL */
    END_TEST();
}

static void test_mult_signed_negative(void)
{
    /* Signed multiplication with negative values */
    BEGIN_TEST("mult_signed_neg");
    SET_REG(R_V0, (uint32_t)(-3));         /* -3 */
    SET_REG(R_V1, (uint32_t)(-4));         /* -4 */
    EMIT(PSX_MULT(R_V0, R_V1));            /* (-3)*(-4) = 12 */
    EMIT(PSX_MFLO(R_A0));
    EMIT(PSX_MFHI(R_A1));
    RUN(2000);
    EXPECT_REG(R_A0, 12);
    EXPECT_REG(R_A1, 0);
    END_TEST();
}

/* ================================================================
 *  REGIMM branches (BLTZ/BGEZ)
 * ================================================================ */

static void test_bltz_bgez(void)
{
    BEGIN_TEST("bltz_bgez");
    SET_REG(R_V0, (uint32_t)(-5));
    SET_REG(R_V1, 5);

    /* BLTZ with negative → taken, skip over ADDIU a0,zero,99 */
    EMIT(PSX_BLTZ(R_V0, 2));               /* 0: taken (v0 < 0) */
    EMIT(PSX_NOP());                        /* 1: delay */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 99));     /* 2: skipped */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 11));     /* 3: target */

    /* BGEZ with positive → taken */
    EMIT(PSX_BGEZ(R_V1, 2));               /* 4: taken (v1 >= 0) */
    EMIT(PSX_NOP());                        /* 5: delay */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 99));     /* 6: skipped */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 22));     /* 7: target */

    RUN(3000);
    EXPECT_REG(R_A0, 11);
    EXPECT_REG(R_A1, 22);
    END_TEST();
}

static void test_blez_bgtz(void)
{
    BEGIN_TEST("blez_bgtz");
    SET_REG(R_V0, 0);
    SET_REG(R_V1, 5);

    /* BLEZ with zero → taken */
    EMIT(PSX_BLEZ(R_V0, 2));               /* 0: taken (v0 <= 0) */
    EMIT(PSX_NOP());                        /* 1: delay */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 99));     /* 2: skipped */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 33));     /* 3: target */

    /* BGTZ with positive → taken */
    EMIT(PSX_BGTZ(R_V1, 2));               /* 4: taken (v1 > 0) */
    EMIT(PSX_NOP());                        /* 5: delay */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 99));     /* 6: skipped */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 44));     /* 7: target */

    RUN(3000);
    EXPECT_REG(R_A0, 33);
    EXPECT_REG(R_A1, 44);
    END_TEST();
}

/* ================================================================
 *  HI/LO register management
 * ================================================================ */

static void test_mthi_mtlo(void)
{
    BEGIN_TEST("mthi_mtlo");
    SET_REG(R_V0, 0xAAAAAAAA);
    SET_REG(R_V1, 0x55555555);
    EMIT(PSX_MTHI(R_V0));
    EMIT(PSX_MTLO(R_V1));
    EMIT(PSX_MFHI(R_A0));
    EMIT(PSX_MFLO(R_A1));
    RUN(1000);
    EXPECT_REG(R_A0, 0xAAAAAAAA);
    EXPECT_REG(R_A1, 0x55555555);
    END_TEST();
}

/* ================================================================
 *  Block System Tests (tests 38+)
 *
 *  These verify cross-block register state, pinned/non-pinned
 *  register preservation, super-block behaviour, and provide the
 *  baseline for future dirty-tracking optimizations.
 * ================================================================ */

static void test_pinned_regs_cross_block(void)
{
    /* Set all 10 pinned PSX registers, branch to a new block, verify all
     * survive.  Pinned regs: v0(2), v1(3), a0(4), a1(5), a2(6),
     * s0(16), s1(17), gp(28), sp(29), ra(31). */
    BEGIN_TEST("pinned_cross_block");
    SET_REG(R_V0, 0xAAAA0002);
    SET_REG(R_V1, 0xAAAA0003);
    SET_REG(R_A0, 0xAAAA0004);
    SET_REG(R_A1, 0xAAAA0005);
    SET_REG(R_A2, 0xAAAA0006);
    SET_REG(R_S0, 0xAAAA0010);
    SET_REG(R_S1, 0xAAAA0011);
    SET_REG(R_GP, 0xAAAA001C);
    SET_REG(R_SP, 0xAAAA001D);
    /* Note: $ra is set to HALT by BEGIN_TEST, we override it here */
    SET_REG(R_RA, PG_HALT_BASE);

    /* Block 1: J to block 2 (forces new block) */
    uint32_t block2_pc = PG_CODE_BASE + 16 * 4;  /* insn 16 */
    EMIT(PSX_J((block2_pc >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    /* Fill gap with NOPs so block 2 is at a known offset */
    for (int i = 2; i < 16; i++) EMIT(PSX_NOP());

    /* Block 2: Verify all pinned regs by storing them to data area.
     * We can't EXPECT_REG here (that's C code), but we can store each
     * reg to memory and check later. Just let them survive to the end. */
    /* Use a simple ADDU $zero, $v0, $zero → the block must read the
     * pinned v0 register. But we just need the regs to survive to RUN exit. */
    EMIT(PSX_NOP()); /* block 2 starts here */

    RUN(5000);
    EXPECT_REG(R_V0, 0xAAAA0002);
    EXPECT_REG(R_V1, 0xAAAA0003);
    EXPECT_REG(R_A0, 0xAAAA0004);
    EXPECT_REG(R_A1, 0xAAAA0005);
    EXPECT_REG(R_A2, 0xAAAA0006);
    EXPECT_REG(R_S0, 0xAAAA0010);
    EXPECT_REG(R_S1, 0xAAAA0011);
    EXPECT_REG(R_GP, 0xAAAA001C);
    EXPECT_REG(R_SP, 0xAAAA001D);
    END_TEST();
}

static void test_nonpinned_regs_cross_block(void)
{
    /* Test non-pinned registers survive across a block boundary.
     * Non-pinned: t0(8), t1(9), t2(10), t3(11), t4(12), t5(13),
     * t6(14), t7(15), s2(18), s3(19), s4(20), s5(21), s6(22),
     * s7(23), t8(24), t9(25), k0(26), k1(27), fp(30), at(1). */
    BEGIN_TEST("nonpinned_cross_block");

    /* Set a selection of non-pinned registers */
    SET_REG(R_T0, 0xBBBB0008);
    SET_REG(R_T1, 0xBBBB0009);
    SET_REG(R_T2, 0xBBBB000A);
    SET_REG(R_T3, 0xBBBB000B);
    SET_REG(R_T4, 0xBBBB000C);
    SET_REG(R_T5, 0xBBBB000D);
    SET_REG(R_S2, 0xBBBB0012);
    SET_REG(R_S3, 0xBBBB0013);
    SET_REG(R_S4, 0xBBBB0014);
    SET_REG(R_S5, 0xBBBB0015);

    /* Block 1: jump forward to force block boundary */
    uint32_t block2_pc = PG_CODE_BASE + 8 * 4;
    EMIT(PSX_J((block2_pc >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    for (int i = 2; i < 8; i++) EMIT(PSX_NOP());

    /* Block 2: touch a few regs to force the JIT to load them */
    EMIT(PSX_ADDU(R_T0, R_T0, R_ZERO));  /* t0 = t0 (force read) */
    EMIT(PSX_ADDU(R_T1, R_T1, R_ZERO));  /* t1 = t1 */
    EMIT(PSX_ADDU(R_S2, R_S2, R_ZERO));  /* s2 = s2 */

    RUN(5000);
    EXPECT_REG(R_T0, 0xBBBB0008);
    EXPECT_REG(R_T1, 0xBBBB0009);
    EXPECT_REG(R_T2, 0xBBBB000A);
    EXPECT_REG(R_T3, 0xBBBB000B);
    EXPECT_REG(R_T4, 0xBBBB000C);
    EXPECT_REG(R_T5, 0xBBBB000D);
    EXPECT_REG(R_S2, 0xBBBB0012);
    EXPECT_REG(R_S3, 0xBBBB0013);
    EXPECT_REG(R_S4, 0xBBBB0014);
    EXPECT_REG(R_S5, 0xBBBB0015);
    END_TEST();
}

static void test_reg_write_cross_block(void)
{
    /* Write to a register in block 1, read it in block 2.
     * This verifies the write-through / dirty flush path. */
    BEGIN_TEST("reg_write_cross_block");
    SET_REG(R_V0, 100);
    SET_REG(R_V1, 200);

    /* Block 1: compute a0 = v0 + v1, then branch */
    EMIT(PSX_ADDU(R_A0, R_V0, R_V1));     /* a0 = 300 */
    EMIT(PSX_ADDIU(R_T0, R_ZERO, 42));    /* t0 = 42 (non-pinned) */
    uint32_t block2_pc = PG_CODE_BASE + 8 * 4;
    EMIT(PSX_J((block2_pc >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    for (int i = 4; i < 8; i++) EMIT(PSX_NOP());

    /* Block 2: use a0 and t0 — must reflect block 1's writes */
    EMIT(PSX_ADDU(R_A1, R_A0, R_T0));     /* a1 = 300 + 42 = 342 */

    RUN(5000);
    EXPECT_REG(R_A0, 300);
    EXPECT_REG(R_T0, 42);
    EXPECT_REG(R_A1, 342);
    END_TEST();
}

static void test_multi_block_chain(void)
{
    /* Chain: Block A → Block B → Block C.
     * Each block modifies a register. Verify all survive. */
    BEGIN_TEST("multi_block_chain");
    SET_REG(R_V0, 10);

    /* Block A starts at PG_CODE_BASE */
    uint32_t block_b = PG_CODE_BASE + 8 * 4;
    uint32_t block_c = PG_CODE_BASE + 16 * 4;

    EMIT(PSX_ADDIU(R_A0, R_V0, 1));       /* a0 = 11 */
    EMIT(PSX_J((block_b >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    for (int i = 3; i < 8; i++) EMIT(PSX_NOP());

    /* Block B */
    EMIT(PSX_ADDIU(R_A1, R_A0, 2));       /* a1 = 13 */
    EMIT(PSX_J((block_c >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    for (int i = 11; i < 16; i++) EMIT(PSX_NOP());

    /* Block C */
    EMIT(PSX_ADDIU(R_A2, R_A1, 3));       /* a2 = 16 */

    RUN(10000);
    EXPECT_REG(R_A0, 11);
    EXPECT_REG(R_A1, 13);
    EXPECT_REG(R_A2, 16);
    END_TEST();
}

static void test_super_block_fallthrough(void)
{
    /* BEQ not taken → super-block fall-through.
     * Verify const propagation and register state across the
     * continuation. */
    BEGIN_TEST("super_block_fallthru");
    SET_REG(R_V0, 1);
    SET_REG(R_V1, 2);  /* v0 != v1 → BEQ not taken */

    /* insn 0: BEQ offset=5 → target=(delay+20)=insn6 */
    EMIT(PSX_BEQ(R_V0, R_V1, 5));
    EMIT(PSX_NOP());                       /* 1: delay slot */

    /* Fall-through path (insns 2-4): */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 100));   /* 2: a0 = 100 */
    EMIT(PSX_ADDIU(R_A1, R_A0, 50));      /* 3: a1 = 150 */
    EMIT(PSX_JR(R_RA));                    /* 4: exit fall-through */
    EMIT(PSX_NOP());                       /* 5: delay */

    /* Taken target (insn 6) — should NOT be reached */
    EMIT(PSX_ADDIU(R_A2, R_ZERO, 999));

    RUN(5000);
    EXPECT_REG(R_A0, 100);
    EXPECT_REG(R_A1, 150);
    EXPECT_REG(R_A2, 0);  /* not reached */
    END_TEST();
}

static void test_super_block_taken(void)
{
    /* BEQ taken → cold deferred path.
     * Verify correct register state on the taken path. */
    BEGIN_TEST("super_block_taken");
    SET_REG(R_V0, 5);
    SET_REG(R_V1, 5);  /* v0 == v1 → BEQ taken */

    /* insn 0: BEQ taken → skip to insn 5 */
    EMIT(PSX_BEQ(R_V0, R_V1, 3));         /* +3 insns from delay slot */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 11));    /* delay: a0 = 11 (always executes) */

    /* insn 2-4: fall-through (skipped) */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 99));    /* skipped */
    EMIT(PSX_ADDIU(R_A2, R_ZERO, 99));    /* skipped */
    EMIT(PSX_NOP());                       /* skipped */

    /* insn 5: taken target */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 22));    /* a1 = 22 */
    EMIT(PSX_ADDIU(R_A2, R_A0, 33));      /* a2 = 11 + 33 = 44 */

    RUN(5000);
    EXPECT_REG(R_A0, 11);   /* delay slot */
    EXPECT_REG(R_A1, 22);   /* taken target */
    EXPECT_REG(R_A2, 44);   /* uses delay slot value */
    END_TEST();
}

static void test_nested_jal(void)
{
    /* Nested subroutine: main → sub1 → sub2 → return chain.
     * Tests $ra save/restore across JAL/JR levels.
     *
     * Layout:
     *  0: LUI t0, 0x8002           (data base)
     *  1: SW  ra, 4(t0)            (save original ra at data+4)
     *  2: JAL sub1                  ($ra = insn 4 addr)
     *  3: NOP                       (delay)
     *  4: ADDIU a2, zero, 33       (after return from sub1)
     *  5: LUI t0, 0x8002
     *  6: LW  ra, 4(t0)            (restore original ra → PG_HALT_BASE)
     *  7: NOP  (pad)
     *  ...pad 8-11...
     *
     * sub1 at insn 12:
     * 12: LUI t0, 0x8002
     * 13: SW  ra, 0(t0)            (save main's ra at data+0)
     * 14: JAL sub2                  ($ra = insn 16 addr)
     * 15: NOP                       (delay)
     * 16: ADDIU a1, zero, 22
     * 17: LUI t0, 0x8002
     * 18: LW  ra, 0(t0)            (restore main's ra)
     * 19: JR  ra                    (return to main insn 4)
     * 20: NOP                       (delay)
     *  ...pad 21-23...
     *
     * sub2 at insn 24:
     * 24: ADDIU a0, zero, 11
     * 25: JR ra                     (return to sub1 insn 16)
     * 26: NOP                       (delay)
     */
    BEGIN_TEST("nested_jal");

    uint32_t sub1_addr = PG_CODE_BASE + 12 * 4;
    uint32_t sub2_addr = PG_CODE_BASE + 24 * 4;

    /* Main block */
    EMIT(PSX_LUI(R_T0, 0x8002));                    /* 0 */
    EMIT(PSX_SW(R_RA, 4, R_T0));                    /* 1: save original ra */
    EMIT(PSX_JAL((sub1_addr >> 2) & 0x03FFFFFF));   /* 2: JAL sub1 */
    EMIT(PSX_NOP());                                 /* 3: delay */
    EMIT(PSX_ADDIU(R_A2, R_ZERO, 33));              /* 4: a2=33 */
    EMIT(PSX_LUI(R_T0, 0x8002));                    /* 5 */
    EMIT(PSX_LW(R_RA, 4, R_T0));                    /* 6: restore original ra */
    EMIT(PSX_JR(R_RA));                              /* 7: exit to halt */
    EMIT(PSX_NOP());                                 /* 8: delay */
    for (int i = 9; i < 12; i++) EMIT(PSX_NOP());   /* pad */

    /* sub1 */
    EMIT(PSX_LUI(R_T0, 0x8002));                    /* 12 */
    EMIT(PSX_SW(R_RA, 0, R_T0));                    /* 13: save main's ra */
    EMIT(PSX_JAL((sub2_addr >> 2) & 0x03FFFFFF));   /* 14: JAL sub2 */
    EMIT(PSX_NOP());                                 /* 15: delay */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 22));              /* 16: a1=22 */
    EMIT(PSX_LUI(R_T0, 0x8002));                    /* 17 */
    EMIT(PSX_LW(R_RA, 0, R_T0));                    /* 18: restore main's ra */
    EMIT(PSX_JR(R_RA));                              /* 19: return to main */
    EMIT(PSX_NOP());                                 /* 20: delay */
    for (int i = 21; i < 24; i++) EMIT(PSX_NOP());  /* pad */

    /* sub2 */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 11));              /* 24: a0=11 */
    EMIT(PSX_JR(R_RA));                              /* 25: return to sub1 */
    EMIT(PSX_NOP());                                 /* 26: delay */

    RUN(30000);
    EXPECT_REG(R_A0, 11);   /* set by sub2 */
    EXPECT_REG(R_A1, 22);   /* set by sub1 */
    EXPECT_REG(R_A2, 33);   /* set after return to main */
    END_TEST();
}

static void test_loop_accumulate_memory(void)
{
    /* Loop that modifies both registers and memory each iteration.
     * Verifies cross-block state for loops with memory side-effects. */
    BEGIN_TEST("loop_accum_mem");

    /* Set up: counter=3, accumulator=0, data area cleared */
    SET_REG(R_V0, 3);     /* counter */
    SET_REG(R_A0, 0);     /* accumulator */

    /* loop:
     *   0: ADDIU a0, a0, 10          accumulate
     *   1: LUI   t0, 0x8002          data base
     *   2: SW    a0, 0(t0)           store current acc to mem
     *   3: ADDIU v0, v0, -1          counter--
     *   4: BNE   v0, zero, -5        branch back to insn 0
     *   5: NOP                        delay
     */
    EMIT(PSX_ADDIU(R_A0, R_A0, 10));
    EMIT(PSX_LUI(R_T0, 0x8002));
    EMIT(PSX_SW(R_A0, 0, R_T0));
    EMIT(PSX_ADDIU(R_V0, R_V0, (uint16_t)(-1)));
    EMIT(PSX_BNE(R_V0, R_ZERO, (uint16_t)(-5)));
    EMIT(PSX_NOP());

    RUN(50000);
    EXPECT_REG(R_A0, 30);    /* 3 × 10 = 30 */
    EXPECT_REG(R_V0, 0);     /* counter reached 0 */
    EXPECT_MEM32(PG_DATA_OFFSET, 30);  /* last store was acc=30 */
    END_TEST();
}

static void test_conditional_both_paths(void)
{
    /* Run the same code with two different initial conditions.
     * Path 1: v0==v1 → taken
     * Path 2: v0!=v1 → not taken
     * Verifies the JIT works correctly for both paths of the SAME block. */
    /* Path 1: BEQ taken (v0==v1). Offset=4 → target=(delay+16)=insn5 */
    BEGIN_TEST("cond_path_taken");
    SET_REG(R_V0, 7);
    SET_REG(R_V1, 7);
    EMIT(PSX_BEQ(R_V0, R_V1, 4));         /* 0: BEQ taken */
    EMIT(PSX_NOP());                       /* 1: delay */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 1));     /* 2: fall-through value */
    EMIT(PSX_JR(R_RA));                    /* 3: exit fall-through */
    EMIT(PSX_NOP());                       /* 4: delay */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 2));     /* 5: taken value */
    RUN(5000);
    EXPECT_REG(R_A0, 2);
    END_TEST();

    /* Path 2: BEQ not taken (v0!=v1). Same layout, different result. */
    BEGIN_TEST("cond_path_not_taken");
    SET_REG(R_V0, 7);
    SET_REG(R_V1, 8);
    EMIT(PSX_BEQ(R_V0, R_V1, 4));         /* 0: BEQ not taken */
    EMIT(PSX_NOP());                       /* 1: delay */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 1));     /* 2: fall-through value */
    EMIT(PSX_JR(R_RA));                    /* 3: exit fall-through */
    EMIT(PSX_NOP());                       /* 4: delay */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 2));     /* 5: taken target (not reached) */
    RUN(5000);
    EXPECT_REG(R_A0, 1);
    END_TEST();
}

static void test_all_32_regs(void)
{
    /* Comprehensive: set all 31 non-zero GPRs, execute a block,
     * verify all survive. $zero is always 0. $ra is special (halt). */
    BEGIN_TEST("all_32_regs");

    /* Set regs 1-30 to distinct values. $ra(31) kept as halt. */
    for (int i = 1; i <= 30; i++)
        cpu.regs[i] = 0xA0000000u | (uint32_t)i;

    /* Keep $ra = halt so JR $ra exits cleanly */
    cpu.regs[R_RA] = PG_HALT_BASE;
    /* Keep $sp reasonable for stack operations in prologue */
    cpu.regs[R_SP] = 0x801FFF00u;

    /* A single NOP block — just compiles and exits.
     * The prologue loads pinned regs, epilogue flushes them. */
    EMIT(PSX_NOP());

    RUN(2000);

    EXPECT_REG(R_ZERO, 0);
    EXPECT_REG(R_AT, 0xA0000001);
    EXPECT_REG(R_V0, 0xA0000002);
    EXPECT_REG(R_V1, 0xA0000003);
    EXPECT_REG(R_A0, 0xA0000004);
    EXPECT_REG(R_A1, 0xA0000005);
    EXPECT_REG(R_A2, 0xA0000006);
    EXPECT_REG(R_A3, 0xA0000007);
    EXPECT_REG(R_T0, 0xA0000008);
    EXPECT_REG(R_T1, 0xA0000009);
    EXPECT_REG(R_T2, 0xA000000A);
    EXPECT_REG(R_T3, 0xA000000B);
    EXPECT_REG(R_T4, 0xA000000C);
    EXPECT_REG(R_T5, 0xA000000D);
    EXPECT_REG(R_T6, 0xA000000E);
    EXPECT_REG(R_T7, 0xA000000F);
    EXPECT_REG(R_S0, 0xA0000010);
    EXPECT_REG(R_S1, 0xA0000011);
    EXPECT_REG(R_S2, 0xA0000012);
    EXPECT_REG(R_S3, 0xA0000013);
    EXPECT_REG(R_S4, 0xA0000014);
    EXPECT_REG(R_S5, 0xA0000015);
    EXPECT_REG(R_S6, 0xA0000016);
    EXPECT_REG(R_S7, 0xA0000017);
    EXPECT_REG(R_T8, 0xA0000018);
    EXPECT_REG(R_T9, 0xA0000019);
    /* k0/k1 may be clobbered by exception handling, skip */
    EXPECT_REG(R_GP, 0xA000001C);
    EXPECT_REG(R_SP, 0x801FFF00u); /* we set this specially */
    EXPECT_REG(R_FP, 0xA000001E);
    END_TEST();
}

/* ================================================================
 *  Test Runner (updated)
 * ================================================================ */

void pg_run_all_tests(void)
{
    printf("--- ALU Basic ---\n");
    test_addu_basic();
    test_addu_overflow_wrap();
    test_subu_basic();
    test_and_or_xor_nor();
    test_addiu_signext();
    test_lui_ori();
    test_andi_xori();
    test_addu_zero_reg();

    printf("\n--- Shifts ---\n");
    test_sll_srl_sra();
    test_sllv_srlv_srav();
    test_shift_by_zero();
    test_sra_sign_extend();

    printf("\n--- Multiply / Divide ---\n");
    test_mult_basic();
    test_multu_basic();
    test_div_basic();
    test_divu_basic();
    test_mult_signed_negative();

    printf("\n--- Comparisons ---\n");
    test_slt_signed();
    test_sltu_unsigned();
    test_slti_sltiu();

    printf("\n--- Load / Store ---\n");
    test_lw_sw_basic();
    test_lb_sb_signext();
    test_lh_sh();
    test_sw_lw_offset();
    test_lwl_lwr();
    test_swl_swr();

    printf("\n--- Branches ---\n");
    test_beq_taken();
    test_beq_not_taken();
    test_bne_taken();
    test_branch_delay_slot();
    test_branch_delay_store();
    test_bltz_bgez();
    test_blez_bgtz();

    printf("\n--- Instruction Interactions ---\n");
    test_store_load_forwarding();
    test_loop_counter();
    test_jal_jr_ra();
    test_mthi_mtlo();

    printf("\n--- Block System ---\n");
    test_pinned_regs_cross_block();
    test_nonpinned_regs_cross_block();
    test_reg_write_cross_block();
    test_multi_block_chain();
    test_super_block_fallthrough();
    test_super_block_taken();
    test_nested_jal();
    test_loop_accumulate_memory();
    test_conditional_both_paths();
    test_all_32_regs();
}
