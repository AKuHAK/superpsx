/*
 * JIT Playground — Main Entry Point
 *
 * Standalone ELF that initialises the JIT engine, then runs a suite of
 * micro-tests against it.  Each test injects R3000A opcodes into PSX
 * RAM, compiles + executes them through the real dynarec, and checks
 * the resulting CPU / memory state.
 *
 * Built as a separate CMake target that links the same dynarec, cpu
 * and memory modules as the main emulator.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <kernel.h>        /* FlushCache */

#include "playground.h"

/* ================================================================
 *  Globals used by the framework
 * ================================================================ */
PlaygroundResults pg_results = {0, 0, 0};
PGTestCtx pg_ctx;

/* ================================================================
 *  Minimal stubs for subsystems the dynarec references but
 *  the playground never exercises.
 * ================================================================ */

/* --- Hardware / IO --- */
uint32_t ReadHardware(uint32_t addr)               { (void)addr; return 0; }
void     WriteHardware(uint32_t addr,
                       uint32_t data, int width)    { (void)addr; (void)data; (void)width; }
void     SignalInterrupt(uint32_t irq)                { (void)irq; }

/* --- GPU --- */
uint32_t gpu_stat = 0;
uint32_t gpu_busy_until = 0;
int      gpu_pending_vblank_flush = 0;
volatile uint64_t gpu_irq_delay_cycle = 0;
void     GPU_VBlank(void)                           {}
uint32_t GPU_ReadStatus(void)                       { return gpu_stat; }
void     GPU_DMA_Write(uint32_t a, int n)           { (void)a; (void)n; }
void     GPU_Write_GP0(uint32_t v)                  { (void)v; }
void     GPU_Write_GP1(uint32_t v)                  { (void)v; }
uint32_t GPU_Read_GPUREAD(void)                     { return 0; }
void     DumpVRAM(void)                             {}

/* --- SPU --- */
void     SPU_GenerateSamples(int n)                 { (void)n; }
void     SPU_Init(void)                             {}
void     SPU_Shutdown(void)                         {}
void     SPU_WriteRegister(uint32_t a, uint16_t v)  { (void)a; (void)v; }
uint16_t SPU_ReadRegister(uint32_t a)               { (void)a; return 0; }
void     SPU_DMA_Write(uint32_t a, int n)           { (void)a; (void)n; }
void     SPU_DMA_Read(uint32_t a, int n)            { (void)a; (void)n; }

/* --- SIO --- */
uint32_t sio_data = 0;
int      sio_tx_pending = 0;
int      sio_selected = 0;
volatile uint64_t sio_irq_delay_cycle = 0;
uint32_t SIO_Read(uint32_t a)                       { (void)a; return 0; }
void     SIO_Write(uint32_t a, uint32_t v)          { (void)a; (void)v; }

/* --- CDROM --- */
uint8_t  cdrom_irq_active = 0;

/* --- Timers --- */
void     Timer_ScheduleAll(void)                    {}

/* --- GTE --- */
void     GTE_Execute(uint32_t op, R3000CPU *c)      { (void)op; (void)c; }
uint32_t GTE_ReadData(R3000CPU *c, int reg)          { (void)c; (void)reg; return 0; }
void     GTE_WriteData(R3000CPU *c, int reg, uint32_t v) { (void)c; (void)reg; (void)v; }
uint32_t GTE_ReadCtrl(R3000CPU *c, int reg)          { (void)c; (void)reg; return 0; }
void     GTE_WriteCtrl(R3000CPU *c, int reg, uint32_t v) { (void)c; (void)reg; (void)v; }
void     GTE_VBlankUpdate(void)                     {}
/* GTE Inline wrappers — signatures must match include/superpsx.h exactly */
void GTE_Inline_RTPS(R3000CPU *c, int sf, int lm)  { (void)c; (void)sf; (void)lm; }
void GTE_Inline_NCLIP(R3000CPU *c)                  { (void)c; }
void GTE_Inline_OP(R3000CPU *c, int sf, int lm)    { (void)c; (void)sf; (void)lm; }
void GTE_Inline_DPCS(R3000CPU *c, int sf, int lm)  { (void)c; (void)sf; (void)lm; }
void GTE_Inline_INTPL(R3000CPU *c, int sf, int lm) { (void)c; (void)sf; (void)lm; }
void GTE_Inline_MVMVA(R3000CPU *c, uint32_t pk)    { (void)c; (void)pk; }
void GTE_Inline_NCDS(R3000CPU *c, int sf, int lm)  { (void)c; (void)sf; (void)lm; }
void GTE_Inline_CDP(R3000CPU *c, int sf, int lm)   { (void)c; (void)sf; (void)lm; }
void GTE_Inline_NCDT(R3000CPU *c, int sf, int lm)  { (void)c; (void)sf; (void)lm; }
void GTE_Inline_NCCS(R3000CPU *c, int sf, int lm)  { (void)c; (void)sf; (void)lm; }
void GTE_Inline_CC(R3000CPU *c, int sf, int lm)    { (void)c; (void)sf; (void)lm; }
void GTE_Inline_NCS(R3000CPU *c, int sf, int lm)   { (void)c; (void)sf; (void)lm; }
void GTE_Inline_NCT(R3000CPU *c, int sf, int lm)   { (void)c; (void)sf; (void)lm; }
void GTE_Inline_SQR(R3000CPU *c, int sf, int lm)   { (void)c; (void)sf; (void)lm; }
void GTE_Inline_DCPL(R3000CPU *c, int sf, int lm)  { (void)c; (void)sf; (void)lm; }
void GTE_Inline_DPCT(R3000CPU *c, int sf, int lm)  { (void)c; (void)sf; (void)lm; }
void GTE_Inline_AVSZ3(R3000CPU *c)                  { (void)c; }
void GTE_Inline_AVSZ4(R3000CPU *c)                  { (void)c; }
void GTE_Inline_RTPT(R3000CPU *c, int sf, int lm)  { (void)c; (void)sf; (void)lm; }
void GTE_Inline_GPF(R3000CPU *c, int sf, int lm)   { (void)c; (void)sf; (void)lm; }
void GTE_Inline_GPL(R3000CPU *c, int sf, int lm)   { (void)c; (void)sf; (void)lm; }
void GTE_Inline_NCCT(R3000CPU *c, int sf, int lm)  { (void)c; (void)sf; (void)lm; }

/* --- Loader --- */
int  Load_PSX_EXE(const char *p, uint32_t *pc)     { (void)p; (void)pc; return -1; }
int  Load_PSX_EXE_FromISO(const char *p, uint32_t *pc) { (void)p; (void)pc; return -1; }

/* --- DMA --- */
void DMA_WriteReg(uint32_t a, uint32_t v)           { (void)a; (void)v; }
uint32_t DMA_ReadReg(uint32_t a)                    { (void)a; return 0; }

/* --- Joystick --- */
void Joystick_Init(void)                            {}

/* --- Config --- */
#include "config.h"
PSXConfig psx_config;     /* zero-initialised */

/* --- Profiler --- */
#include "profiler.h"
#ifdef ENABLE_SUBSYSTEM_PROFILER
int prof_disable_spu = 1;
int prof_disable_gpu_render = 1;
/* Minimal profiler stubs — the real profiler.c is not linked */
void profiler_init(void)        {}
void profiler_frame_end(void)   {}
#else
int prof_disable_spu = 1;
int prof_disable_gpu_render = 1;
#endif

/* --- Boot mode (run.c externs) --- */
int         psx_boot_mode = 0;
const char *psx_exe_filename = NULL;

/* ================================================================
 *  JIT execution wrapper
 * ================================================================ */

/* Externs from dynarec */
extern uint32_t *code_buffer;
extern uint32_t *code_ptr;
extern BlockEntry *block_node_pool;
extern int block_node_pool_idx;
extern int patch_sites_count;
extern uint32_t blocks_compiled;
extern uint32_t total_instructions;
extern int tlb_bp_map_count;
extern JitHTEntry jit_ht[];
extern jit_l2_t jit_l1_ram[];
extern jit_l2_t jit_l1_bios[];
extern uint8_t jit_page_gen[];
extern uint32_t psx_tlb_base;

extern uint32_t *compile_block(uint32_t psx_pc);
extern void Free_PageTable(void);
extern void Init_Dynarec(void);
extern void Init_CPU(void);
extern void Init_Memory(void);
extern void Init_MemoryLUT(void);
extern uint32_t psx_abort_pc;

/* Scheduler externs */
extern uint64_t global_cycles;

void pg_reset_jit_cache(void)
{
    /* Reset code pointer past trampolines */
    code_ptr = code_buffer + 144;

    /* Clear page tables */
    Free_PageTable();
    memset(jit_l1_ram, 0, sizeof(jit_l1_ram[0]) * 512);
    memset(jit_l1_bios, 0, sizeof(jit_l1_bios[0]) * 128);

    /* Clear block pool */
    block_node_pool_idx = 0;
    patch_sites_count = 0;
    blocks_compiled = 0;
    tlb_bp_map_count = 0;

    /* Clear hash table */
    for (int i = 0; i < JIT_HT_SIZE; i++) {
        jit_ht[i].psx_pc[0] = 0xFFFFFFFF;
        jit_ht[i].psx_pc[1] = 0xFFFFFFFF;
        jit_ht[i].native[0] = NULL;
        jit_ht[i].native[1] = NULL;
    }

    FlushCache(0);
    FlushCache(2);
}

void pg_run_jit(uint32_t pc, int32_t cycles)
{
    cpu.cycles_left = cycles;
    cpu.initial_cycles_left = cycles;
    cpu.block_aborted = 0;
    cpu.pc = pc;
    global_cycles = 0;

    int max_dispatches = 200;

    while (cpu.cycles_left > 0 && max_dispatches-- > 0) {
        uint32_t curr_pc = cpu.pc;

        /* Stop if we've left the test code / halt region */
        uint32_t phys = curr_pc & 0x1FFFFFFF;
        if (phys >= PSX_RAM_SIZE)
            break;

        BlockEntry *be = lookup_block(curr_pc);
        uint32_t *block = be ? be->native : NULL;

        if (!block) {
            block = compile_block(curr_pc);
            if (!block) break;
            be = lookup_block(curr_pc);
            apply_pending_patches(curr_pc, block);
            jit_ht_add(curr_pc, block);
            FlushCache(0);
            FlushCache(2);
        } else {
            uint32_t h = jit_ht_hash(curr_pc);
            if (jit_ht[h].psx_pc[0] != curr_pc)
                jit_ht_add(curr_pc, block);
        }

        block_func_t fn = (block_func_t)block;
        int32_t remaining = fn(&cpu, psx_ram, psx_bios, cpu.cycles_left);
        cpu.cycles_left = remaining;

        if (cpu.block_aborted) {
            cpu.pc = psx_abort_pc;
            cpu.block_aborted = 0;
        }
    }
}

/* ================================================================
 *  Main
 * ================================================================ */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    printf("\n========================================\n");
    printf("   JIT Playground — SuperPSX\n");
    printf("========================================\n\n");

    /* 1. Initialise memory (allocates psx_ram, psx_bios, mem_lut) */
    Init_Memory();
    Init_MemoryLUT();

    /* 2. Initialise CPU struct */
    Init_CPU();

    /* 3. Initialise dynarec (allocates code_buffer, builds trampolines) */
    Init_Dynarec();

    /* 4. Run all tests */
    pg_run_all_tests();

    /* 5. Summary */
    printf("\n========================================\n");
    printf("   Results: %d/%d passed",
           pg_results.passed, pg_results.total);
    if (pg_results.failed > 0)
        printf(", %d FAILED", pg_results.failed);
    printf("\n========================================\n\n");

    /* Return exit code for CI */
    return pg_results.failed > 0 ? 1 : 0;
}
