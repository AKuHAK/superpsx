/*
 * GPU Playground — Deferred State Computation Tests (Optimization C)
 *
 * Tests for the steady-state fast path: when gs_state is valid AND
 * prim_tex_cache hits AND command attributes haven't changed, skip
 * all want_xxx computation and state comparison.
 *
 * Uses DMA block API for clean measurements (no per-word call overhead).
 */
#include "playground_gpu.h"
#include <string.h>

extern uint32_t vram_gen_counter;
extern void Tex_Cache_Init(void);

/* ================================================================
 *  DMA block helper
 * ================================================================ */
#define EMIT_DMA_BLOCK_D(arr, cnt) do { \
    uint32_t _cycles, _insns; \
    perf_start(); \
    GPU_ProcessDmaBlock((arr), (cnt)); \
    perf_stop(&_cycles, &_insns); \
    gp_ctx.eecycles_used += _cycles; \
    gp_ctx.eeinsns_used += _insns; \
} while(0)

/* ================================================================
 *  Shared helpers
 * ================================================================ */
static void write_clut_4bpp_d(int clut_x, int clut_y, uint16_t base_color)
{
    if (!psx_vram_shadow) return;
    for (int i = 0; i < 16; i++)
        psx_vram_shadow[clut_y * 1024 + clut_x + i] = base_color + i;
}

static void write_texpage_data_d(int page_x, int page_y)
{
    if (!psx_vram_shadow) return;
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 128; x++)
            psx_vram_shadow[(page_y + y) * 1024 + page_x + x] = (uint16_t)(x + y * 128);
}

/* Build a single textured quad (0x2C) command into a buffer.
 * Returns number of words written. */
static int build_texquad_4bpp(uint32_t *buf, int page_tx, int page_ty, int clut_x, int clut_y)
{
    uint32_t e1_val = (page_tx & 0xF) | ((page_ty & 1) << 4) | (0 << 7);
    uint32_t clut_word = ((clut_x / 16) & 0x3F) | (((clut_y) & 0x1FF) << 6);
    uint32_t tpage_word = (page_tx & 0xF) | ((page_ty & 1) << 4) | (0 << 7);
    int n = 0;
    buf[n++] = 0xE1000000 | e1_val;
    buf[n++] = 0x2C808080;
    buf[n++] = 10 | (10 << 16);
    buf[n++] = 0 | (0 << 8) | (clut_word << 16);
    buf[n++] = 50 | (10 << 16);
    buf[n++] = 32 | (0 << 8) | (tpage_word << 16);
    buf[n++] = 10 | (50 << 16);
    buf[n++] = 0 | (32 << 8);
    buf[n++] = 50 | (50 << 16);
    buf[n++] = 32 | (32 << 8);
    return n;
}

/* ================================================================
 *  Test D1: DMA burst of 10 identical textured quads
 *
 *  First quad is cold (full state), remaining 9 hit fast path.
 *  DMA block avoids per-word overhead → clean measurement.
 * ================================================================ */
static void test_dma_tex_burst(void)
{
    BEGIN_GPU_TEST("dma_tex10");

    SETUP_GP1(0x00000000);
    Tex_Cache_Init();
    write_texpage_data_d(0, 0);
    write_clut_4bpp_d(0, 240, 0x1000);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 128, 256);
    Tex_Cache_DirtyRegion(0, 240, 16, 1);

    /* Build DMA block: 10 identical textured quads */
    uint32_t block[100];
    int total = 0;
    for (int i = 0; i < 10; i++)
        total += build_texquad_4bpp(block + total, 0, 0, 0, 240);

    EMIT_DMA_BLOCK_D(block, total);
    Flush_GIF();

    printf("    %-16s: %u CYCLES for 10 textured quads via DMA\n",
           gp_ctx.name, (unsigned)gp_ctx.eecycles_used);

    /* 1 cold (includes page upload ~38K) + 9 fast-path (~1500 each).
     * Pre-opt baseline: ~51K. Post-opt: ~50.8K.
     * Cold start dominates — set generous limit. */
    EXPECT_CYCLES(52000);

    END_GPU_TEST();
}

/* ================================================================
 *  Test D2: DMA burst of 10 flat quads
 *
 *  Untextured flat quad fast path.
 * ================================================================ */
static void test_dma_flat_burst(void)
{
    BEGIN_GPU_TEST("dma_flat10");

    SETUP_GP1(0x00000000);
    Tex_Cache_Init();

    /* Build DMA block: 10 flat quads */
    uint32_t block[50];
    int n = 0;
    for (int i = 0; i < 10; i++) {
        block[n++] = 0x28FF0000;  /* Red flat quad */
        block[n++] = 10 | (10 << 16);
        block[n++] = 100 | (10 << 16);
        block[n++] = 10 | (100 << 16);
        block[n++] = 100 | (100 << 16);
    }

    EMIT_DMA_BLOCK_D(block, n);
    Flush_GIF();

    printf("    %-16s: %u CYCLES for 10 flat quads via DMA\n",
           gp_ctx.name, (unsigned)gp_ctx.eecycles_used);

    /* Flat fast path saves little (no GS_SET_TEX0 to skip).
     * Baseline: ~3400. Set generous limit. */
    EXPECT_CYCLES(3500);

    END_GPU_TEST();
}

/* ================================================================
 *  Test D3: DMA steady-state single textured quad
 *
 *  Emit one quad to warm state, then measure the second separately.
 * ================================================================ */
static void test_dma_steady_texq(void)
{
    BEGIN_GPU_TEST("dma_ss_txq");

    SETUP_GP1(0x00000000);
    Tex_Cache_Init();
    write_texpage_data_d(0, 0);
    write_clut_4bpp_d(0, 240, 0x1000);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 128, 256);
    Tex_Cache_DirtyRegion(0, 240, 16, 1);

    /* Warm-up: first quad (cold, not measured) */
    uint32_t warmup[10];
    int wn = build_texquad_4bpp(warmup, 0, 0, 0, 240);
    GPU_ProcessDmaBlock(warmup, wn);
    Flush_GIF();

    /* Reset counters */
    gp_ctx.eecycles_used = 0;
    gp_ctx.eeinsns_used = 0;
    gp_ctx.qwords_generated = 0;
    mock_gif_qwords_written = 0;
    fast_gif_ptr = MOCK_GIF_BUFFER_START;

    /* Measure: second quad (steady-state, fast path) */
    uint32_t steady[10];
    int sn = build_texquad_4bpp(steady, 0, 0, 0, 240);
    EMIT_DMA_BLOCK_D(steady, sn);
    Flush_GIF();

    printf("    %-16s: %u CYCLES for steady-state textured quad\n",
           gp_ctx.name, (unsigned)gp_ctx.eecycles_used);

    /* Steady-state should skip all state computation via fast path.
     * Pre-opt: ~1674 cycles (bug: fast path didn't trigger).
     * Post-opt: ~1508 cycles (fast path active).
     * Target: < 1600 */
    EXPECT_CYCLES(1600);

    END_GPU_TEST();
}

/* ================================================================
 *  Runner
 * ================================================================ */
void gp_run_deferred_tests(void)
{
    printf("─── Deferred State Computation Tests (C) ──────────────────\n");
    test_dma_tex_burst();
    test_dma_flat_burst();
    test_dma_steady_texq();
    printf("\n");
}

