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
 *  Test Runner
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
}
