/*
 * JIT Playground — Expansion Ratio Tests
 *
 * Measures the code expansion ratio (native EE words per PSX instruction)
 * for each instruction category.  This is compile-only: blocks are
 * compiled but NOT executed.
 *
 * The output is a table:
 *   <instruction>  <psx_count> PSX → <native_count> EE  (<ratio>x)
 *
 * The expansion ratio is the key metric for JIT code density optimization.
 * Lower is better — PSX and EE share the MIPS ISA, so the ideal ratio
 * for simple ALU ops is ~1.0x (1 native word per PSX instruction).
 *
 * NOTE: native_count includes per-block overhead (prologue, epilogue,
 * slot load/store, cycle accounting).  To isolate per-instruction cost,
 * compare blocks of different sizes.
 */
#include "playground.h"
#include <string.h>

/* ---- Externs from dynarec ---- */
extern uint32_t *dynarec_ensure_block(uint32_t pc, BlockEntry **out_be);

/* ---- Helper: enable COP2 in SR (needed for GTE instructions) ---- */
static void expansion_enable_cop2(void)
{
    cpu.cop0[PSX_COP0_SR_IDX] = (1u << 30) | (1u << 28);
}

/* ================================================================
 *  Core measurement function
 *
 *  Places `count` repetitions of `insn` into code area, appends
 *  JR $ra + NOP, compiles via dynarec, and reports native_count.
 *
 *  If setup_fn is non-NULL, it's called before compilation to set
 *  up CPU state (e.g., enable COP2 in SR).
 *
 *  Returns native_count, or -1 on error.
 * ================================================================ */
typedef void (*setup_fn_t)(void);

static int measure_block(const uint32_t *insns, int insn_types,
                          int repeat, const char *name,
                          setup_fn_t setup)
{
    /* Reset CPU state */
    memset(&cpu, 0, sizeof(cpu));
    cpu.regs[R_SP] = 0x801FFF00u;
    cpu.regs[R_RA] = PG_HALT_BASE;

    /* Enable COP0 always (game-like SR value) */
    cpu.cop0[PSX_COP0_SR_IDX] = (1u << 28); /* CU0 enable */

    if (setup) setup();

    /* Reset JIT cache */
    pg_reset_jit_cache();

    /* Place instructions: repeat the pattern `repeat` times */
    uint32_t *code = (uint32_t *)(psx_ram + PG_CODE_OFFSET);
    memset(code, 0, 4096);
    int total_insns = 0;
    for (int r = 0; r < repeat; r++) {
        for (int i = 0; i < insn_types; i++) {
            if (total_insns >= PG_MAX_INSN - 2) break;
            code[total_insns++] = insns[i];
        }
    }
    /* JR $ra + NOP epilogue */
    code[total_insns]     = PSX_JR(R_RA);
    code[total_insns + 1] = PSX_NOP();

    /* Install halt loop */
    memset(psx_ram + PG_HALT_OFFSET, 0, 256);
    {
        uint32_t *halt = (uint32_t *)(psx_ram + PG_HALT_OFFSET);
        halt[0] = MK_I(0x04, 0, 0, (uint16_t)(-1));
        halt[1] = 0x00000000u;
    }

    /* Compile (but don't run) */
    BlockEntry *be = NULL;
    uint32_t *block = dynarec_ensure_block(PG_CODE_BASE, &be);
    if (!block || !be) {
        printf("  %-20s  COMPILE FAILED\n", name);
        return -1;
    }

    int psx_n = (int)be->instr_count;
    int ee_n  = (int)be->native_count;
    float ratio = (psx_n > 0) ? (float)ee_n / (float)psx_n : 0.0f;

    printf("  %-20s  %3d PSX -> %4d EE  (%4.1fx)\n",
           name, psx_n, ee_n, ratio);

    return ee_n;
}

/* Convenience: measure a single repeated instruction */
static int measure_single(uint32_t insn, int repeat, const char *name,
                           setup_fn_t setup)
{
    return measure_block(&insn, 1, repeat, name, setup);
}

/* ================================================================
 *  Expansion ratio measurements — grouped by category
 * ================================================================ */

#define REPEAT 16  /* Number of instruction repetitions per block */

/* ---- ALU R-type ---- */
static void measure_alu_rtype(void)
{
    printf("\n  --- ALU R-type ---\n");
    measure_single(PSX_ADDU(R_T1, R_T2, R_T3), REPEAT, "ADDU", NULL);
    measure_single(PSX_SUBU(R_T1, R_T2, R_T3), REPEAT, "SUBU", NULL);
    measure_single(PSX_AND(R_T1, R_T2, R_T3),  REPEAT, "AND",  NULL);
    measure_single(PSX_OR(R_T1, R_T2, R_T3),   REPEAT, "OR",   NULL);
    measure_single(PSX_XOR(R_T1, R_T2, R_T3),  REPEAT, "XOR",  NULL);
    measure_single(PSX_NOR(R_T1, R_T2, R_T3),  REPEAT, "NOR",  NULL);
    measure_single(PSX_SLT(R_T1, R_T2, R_T3),  REPEAT, "SLT",  NULL);
    measure_single(PSX_SLTU(R_T1, R_T2, R_T3), REPEAT, "SLTU", NULL);
}

/* ---- ALU I-type ---- */
static void measure_alu_itype(void)
{
    printf("\n  --- ALU I-type ---\n");
    measure_single(PSX_ADDIU(R_T1, R_T2, 42),  REPEAT, "ADDIU", NULL);
    measure_single(PSX_ANDI(R_T1, R_T2, 0xFF), REPEAT, "ANDI",  NULL);
    measure_single(PSX_ORI(R_T1, R_T2, 0xFF),  REPEAT, "ORI",   NULL);
    measure_single(PSX_XORI(R_T1, R_T2, 0xFF), REPEAT, "XORI",  NULL);
    measure_single(PSX_SLTI(R_T1, R_T2, 42),   REPEAT, "SLTI",  NULL);
    measure_single(PSX_SLTIU(R_T1, R_T2, 42),  REPEAT, "SLTIU", NULL);
    measure_single(PSX_LUI(R_T1, 0x8000),      REPEAT, "LUI",   NULL);
}

/* ---- Shifts ---- */
static void measure_shifts(void)
{
    printf("\n  --- Shifts ---\n");
    measure_single(PSX_SLL(R_T1, R_T2, 5),     REPEAT, "SLL",  NULL);
    measure_single(PSX_SRL(R_T1, R_T2, 5),     REPEAT, "SRL",  NULL);
    measure_single(PSX_SRA(R_T1, R_T2, 5),     REPEAT, "SRA",  NULL);
    measure_single(PSX_SLLV(R_T1, R_T2, R_T3), REPEAT, "SLLV", NULL);
    measure_single(PSX_SRLV(R_T1, R_T2, R_T3), REPEAT, "SRLV", NULL);
    measure_single(PSX_SRAV(R_T1, R_T2, R_T3), REPEAT, "SRAV", NULL);
}

/* ---- Multiply / Divide ---- */
static void measure_muldiv(void)
{
    printf("\n  --- Multiply / Divide ---\n");
    measure_single(PSX_MULT(R_T1, R_T2),  REPEAT, "MULT",  NULL);
    measure_single(PSX_MULTU(R_T1, R_T2), REPEAT, "MULTU", NULL);
    measure_single(PSX_DIV(R_T1, R_T2),   REPEAT, "DIV",   NULL);
    measure_single(PSX_DIVU(R_T1, R_T2),  REPEAT, "DIVU",  NULL);
    measure_single(PSX_MFHI(R_T1),         REPEAT, "MFHI",  NULL);
    measure_single(PSX_MFLO(R_T1),         REPEAT, "MFLO",  NULL);
}

/* ---- Load / Store ---- */
static void measure_loadstore(void)
{
    printf("\n  --- Load / Store ---\n");
    /* Use $sp (pinned to S4) as base — always valid RAM address */
    measure_single(PSX_LW(R_T1, 0, R_SP),  REPEAT, "LW",  NULL);
    measure_single(PSX_LH(R_T1, 0, R_SP),  REPEAT, "LH",  NULL);
    measure_single(PSX_LB(R_T1, 0, R_SP),  REPEAT, "LB",  NULL);
    measure_single(PSX_LBU(R_T1, 0, R_SP), REPEAT, "LBU", NULL);
    measure_single(PSX_LHU(R_T1, 0, R_SP), REPEAT, "LHU", NULL);
    measure_single(PSX_SW(R_T1, 0, R_SP),  REPEAT, "SW",  NULL);
    measure_single(PSX_SH(R_T1, 0, R_SP),  REPEAT, "SH",  NULL);
    measure_single(PSX_SB(R_T1, 0, R_SP),  REPEAT, "SB",  NULL);
}

/* ---- COP2 (GTE) ---- */
static void measure_gte(void)
{
    printf("\n  --- COP2 (GTE) ---\n");
    measure_single(PSX_MTC2(R_T1, GTE_VXY0), REPEAT, "MTC2", expansion_enable_cop2);
    measure_single(PSX_MFC2(R_T1, GTE_VXY0), REPEAT, "MFC2", expansion_enable_cop2);
    measure_single(PSX_CTC2(R_T1, GTE_TRX),  REPEAT, "CTC2", expansion_enable_cop2);
    measure_single(PSX_CFC2(R_T1, GTE_TRX),  REPEAT, "CFC2", expansion_enable_cop2);
    measure_single(GTE_CMD_RTPS(1, 1),        REPEAT, "COP2 RTPS", expansion_enable_cop2);
    measure_single(GTE_CMD_NCLIP,             REPEAT, "COP2 NCLIP", expansion_enable_cop2);
}

/* ---- Mixed patterns (realistic game code) ---- */
static void measure_mixed(void)
{
    printf("\n  --- Mixed (typical game patterns) ---\n");

    /* ALU chain: t1 = t2 + t3; t4 = t1 & 0xFF; etc */
    {
        uint32_t alu_chain[] = {
            PSX_ADDU(R_T1, R_T2, R_T3),
            PSX_ANDI(R_T4, R_T1, 0xFF),
            PSX_SLL(R_T5, R_T4, 2),
            PSX_OR(R_T6, R_T5, R_T1),
        };
        measure_block(alu_chain, 4, 4, "ALU chain (4x4)", NULL);
    }

    /* Load-use: LW + ADDU using loaded value */
    {
        uint32_t load_use[] = {
            PSX_LW(R_T1, 0, R_SP),
            PSX_ADDU(R_T2, R_T1, R_T3),
        };
        measure_block(load_use, 2, 8, "LW+use (2x8)", NULL);
    }

    /* GTE transform pattern (Crash-like) */
    {
        uint32_t gte_xform[] = {
            PSX_MTC2(R_T1, GTE_VXY0),
            PSX_MTC2(R_T2, GTE_VZ0),
            GTE_CMD_RTPS(1, 1),
            PSX_MFC2(R_T3, GTE_SXY2),
            PSX_SW(R_T3, 0, R_SP),
        };
        measure_block(gte_xform, 5, 3, "GTE xform (5x3)", expansion_enable_cop2);
    }

    /* Store burst: multiple SWs to sequential offsets */
    {
        uint32_t sw_burst[] = {
            PSX_SW(R_T1, 0, R_SP),
            PSX_SW(R_T2, 4, R_SP),
            PSX_SW(R_T3, 8, R_SP),
            PSX_SW(R_T4, 12, R_SP),
        };
        measure_block(sw_burst, 4, 4, "SW burst (4x4)", NULL);
    }
}

/* ================================================================
 *  Baseline: measure prologue+epilogue overhead
 * ================================================================ */
static void measure_baseline(void)
{
    printf("\n  --- Baseline (prologue + epilogue overhead) ---\n");

    /* Empty block: just JR $ra + NOP (2 PSX insns → minimum native code) */
    measure_single(PSX_NOP(), 0, "empty (JR+NOP)", NULL);

    /* 1 NOP — JIT NOP should be essentially free */
    measure_single(PSX_NOP(), 1, "1x NOP", NULL);

    /* 4 NOPs */
    measure_single(PSX_NOP(), 4, "4x NOP", NULL);

    /* 16 NOPs */
    measure_single(PSX_NOP(), 16, "16x NOP", NULL);
}


/* ================================================================
 *  Category runner
 * ================================================================ */
void pg_run_expansion_tests(void)
{
    printf("\n=== Expansion Ratio Report ===\n");
    printf("  (REPEAT=%d instructions per block unless noted)\n", REPEAT);

    measure_baseline();
    measure_alu_rtype();
    measure_alu_itype();
    measure_shifts();
    measure_muldiv();
    measure_loadstore();
    measure_gte();
    measure_mixed();

    printf("\n=== End Expansion Ratio Report ===\n\n");
}
