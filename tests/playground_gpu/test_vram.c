/*
 * GPU Playground — Shadow VRAM & Transfer Tests
 *
 * Functional tests that verify:
 * 1. FillRect (GP0 0x02) shadow VRAM pixel correctness
 * 2. CPU→VRAM (GP0 A0h) shadow writes
 * 3. VRAM→VRAM copy (GP0 80h) shadow correctness
 * 4. VRAM dirty tracking after transfers
 * 5. GP1(10h) info query responses
 */
#include "playground_gpu.h"
#include <string.h>
#include <gs_gp.h>

extern uint32_t vram_gen_counter;
extern void Tex_Cache_Init(void);

/* ================================================================
 *  V1: FillRect shadow pixel correctness
 * ================================================================ */
static void test_fillrect_shadow(void)
{
    BEGIN_GPU_TEST("fill_shadow");

    /* Clear the area first */
    memset(&psx_vram_shadow[100 * 1024 + 100], 0, 32 * sizeof(uint16_t));

    /* GP0(02): FillRect color=0x00FF00 (R=0,G=255,B=0) at (96,100) 16x16
     * Note: FillRect x is rounded down to 16-pixel boundary,
     * and w is rounded up to 16-pixel boundary.
     * x=96 & 0x3F0 = 96, w=16 → (16+15)&~15 = 16 */
    EMIT_GP0(0x0200FF00);        /* cmd + color (R=0, G=0xFF, B=0) */
    EMIT_GP0(96 | (100 << 16));  /* x=96, y=100 */
    EMIT_GP0(16 | (4 << 16));    /* w=16, h=4 */

    /* PSX 15-bit color: R=0>>3=0, G=255>>3=31, B=0>>3=0
     * psx_color = 0 | (31<<5) | 0 = 0x03E0 */
    EXPECT_VRAM_PIXEL(96, 100, 0x03E0);
    EXPECT_VRAM_PIXEL(97, 100, 0x03E0);
    EXPECT_VRAM_PIXEL(111, 103, 0x03E0); /* last pixel of fill */

    END_GPU_TEST();
}

/* ================================================================
 *  V2: FillRect with red color
 * ================================================================ */
static void test_fillrect_red(void)
{
    BEGIN_GPU_TEST("fill_red");

    /* Color 0x0000FF = R=0xFF, G=0, B=0 in PSX order */
    EMIT_GP0(0x020000FF);        /* R=0xFF */
    EMIT_GP0(0 | (0 << 16));    /* x=0, y=0 */
    EMIT_GP0(16 | (1 << 16));   /* w=16, h=1 */

    /* PSX: R=255>>3=31, G=0, B=0 → 0x001F */
    EXPECT_VRAM_PIXEL(0, 0, 0x001F);
    EXPECT_VRAM_PIXEL(15, 0, 0x001F);

    END_GPU_TEST();
}

/* ================================================================
 *  V3: FillRect with white — all channels
 * ================================================================ */
static void test_fillrect_white(void)
{
    BEGIN_GPU_TEST("fill_white");

    EMIT_GP0(0x02FFFFFF);
    EMIT_GP0(0 | (200 << 16));
    EMIT_GP0(16 | (1 << 16));

    /* PSX: R=31, G=31, B=31 → 0x7FFF */
    EXPECT_VRAM_PIXEL(0, 200, 0x7FFF);

    END_GPU_TEST();
}

/* ================================================================
 *  V4: FillRect with black — zero color
 * ================================================================ */
static void test_fillrect_black(void)
{
    BEGIN_GPU_TEST("fill_black");

    /* Pre-fill with non-zero so we can verify the fill */
    for (int i = 0; i < 16; i++)
        psx_vram_shadow[300 * 1024 + i] = 0x7FFF;

    EMIT_GP0(0x02000000);
    EMIT_GP0(0 | (300 << 16));
    EMIT_GP0(16 | (1 << 16));

    EXPECT_VRAM_PIXEL(0, 300, 0x0000);
    EXPECT_VRAM_PIXEL(15, 300, 0x0000);

    END_GPU_TEST();
}

/* ================================================================
 *  V5: FillRect dirty tracking — fills mark gen dirty
 * ================================================================ */
static void test_fillrect_dirty(void)
{
    BEGIN_GPU_TEST("fill_dirty");
    Tex_Cache_Init();

    uint32_t gen_before = vram_gen_counter;

    EMIT_GP0(0x02808080);
    EMIT_GP0(0 | (0 << 16));
    EMIT_GP0(16 | (1 << 16));

    /* After FillRect, vram_gen_counter must have incremented */
    int dirty = (vram_gen_counter != gen_before);
    if (dirty) {
        printf("    %-16s: gen dirty OK\n", gp_ctx.name);
    } else {
        printf("  [FAIL] %-16s: gen NOT dirty after FillRect\n", gp_ctx.name);
        gp_ctx.fail_count++;
    }

    END_GPU_TEST();
}

/* ================================================================
 *  V6: CPU→VRAM (GP0 A0h) shadow write
 * ================================================================ */
static void test_cpu_to_vram_shadow(void)
{
    BEGIN_GPU_TEST("c2v_shadow");

    /* Clear target area */
    memset(&psx_vram_shadow[400 * 1024], 0, 8 * sizeof(uint16_t));

    /* GP0(A0h) - start CPU→VRAM transfer: x=0, y=400, w=4, h=1 = 4 pixels = 2 words */
    EMIT_GP0(0xA0000000);
    EMIT_GP0(0 | (400 << 16));  /* x=0, y=400 */
    EMIT_GP0(4 | (1 << 16));    /* w=4, h=1 */

    /* Send pixel data: 2 pixels per word */
    EMIT_GP0(0x001F0000);  /* pixel0=0x0000, pixel1=0x001F (red) */
    EMIT_GP0(0x7FFF03E0);  /* pixel2=0x03E0 (green), pixel3=0x7FFF (white) */

    EXPECT_VRAM_PIXEL(0, 400, 0x0000);  /* black */
    EXPECT_VRAM_PIXEL(1, 400, 0x001F);  /* red */
    EXPECT_VRAM_PIXEL(2, 400, 0x03E0);  /* green */
    EXPECT_VRAM_PIXEL(3, 400, 0x7FFF);  /* white */

    END_GPU_TEST();
}

/* ================================================================
 *  V7: VRAM→VRAM copy (GP0 80h) basic
 * ================================================================ */
static void test_vram_copy_basic(void)
{
    BEGIN_GPU_TEST("v2v_basic");

    /* Write a pattern to source area (0,450) 4x1 */
    psx_vram_shadow[450 * 1024 + 0] = 0x1111;
    psx_vram_shadow[450 * 1024 + 1] = 0x2222;
    psx_vram_shadow[450 * 1024 + 2] = 0x3333;
    psx_vram_shadow[450 * 1024 + 3] = 0x4444;

    /* Clear destination (100,450) */
    memset(&psx_vram_shadow[450 * 1024 + 100], 0, 8 * sizeof(uint16_t));

    /* GP0(80h): copy (0,450) → (100,450) 4x1 */
    EMIT_GP0(0x80000000);
    EMIT_GP0(0 | (450 << 16));    /* src: x=0, y=450 */
    EMIT_GP0(100 | (450 << 16));  /* dst: x=100, y=450 */
    EMIT_GP0(4 | (1 << 16));      /* w=4, h=1 */

    EXPECT_VRAM_PIXEL(100, 450, 0x1111);
    EXPECT_VRAM_PIXEL(101, 450, 0x2222);
    EXPECT_VRAM_PIXEL(102, 450, 0x3333);
    EXPECT_VRAM_PIXEL(103, 450, 0x4444);

    END_GPU_TEST();
}

/* ================================================================
 *  V8: VRAM copy dirty tracking
 * ================================================================ */
static void test_vram_copy_dirty(void)
{
    BEGIN_GPU_TEST("v2v_dirty");

    /* Write source data */
    psx_vram_shadow[460 * 1024 + 0] = 0xAAAA;

    uint32_t gen_before = vram_gen_counter;

    /* GP0(80h): copy (0,460)→(100,460) 1x1 */
    EMIT_GP0(0x80000000);
    EMIT_GP0(0 | (460 << 16));
    EMIT_GP0(100 | (460 << 16));
    EMIT_GP0(1 | (1 << 16));

    int dirty = (vram_gen_counter != gen_before);
    if (dirty) {
        printf("    %-16s: copy gen dirty OK\n", gp_ctx.name);
    } else {
        printf("  [FAIL] %-16s: copy did NOT dirty gen\n", gp_ctx.name);
        gp_ctx.fail_count++;
    }

    END_GPU_TEST();
}

/* ================================================================
 *  V9: FillRect then textured draw — page re-upload after fill
 * ================================================================ */
static void test_fill_invalidates_texture(void)
{
    BEGIN_GPU_TEST("fill_inval");
    Tex_Cache_Init();
    vram_gen_counter++;

    /* Setup texture at page (0,0) and CLUT at (0,256) */
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 64; x++)
            psx_vram_shadow[y * 1024 + x] = (uint16_t)(x + y);
    for (int i = 0; i < 16; i++)
        psx_vram_shadow[256 * 1024 + i] = (uint16_t)(0x1000 + i);

    /* Draw textured quad — warms cache */
    uint32_t e1 = (0 & 0xF) | ((0 & 1) << 4) | (0 << 7);
    EMIT_GP0(0xE1000000 | e1);

    uint32_t clut_w = ((0 / 16) & 0x3F) | (((256) & 0x1FF) << 6);
    uint32_t tp_w = (0 & 0xF) | ((0 & 1) << 4) | (0 << 7);
    EMIT_GP0(0x2C808080);
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(0 | (0 << 8) | (clut_w << 16));
    EMIT_GP0(50 | (10 << 16));
    EMIT_GP0(32 | (0 << 8) | (tp_w << 16));
    EMIT_GP0(10 | (50 << 16));
    EMIT_GP0(0 | (32 << 8));
    EMIT_GP0(50 | (50 << 16));
    EMIT_GP0(32 | (32 << 8));
    gp_gif_reset_counter();

    /* FillRect INTO the texture page area — should dirty it */
    EMIT_GP0(0x02FF0000);
    EMIT_GP0(0 | (0 << 16));
    EMIT_GP0(16 | (16 << 16));
    gp_gif_reset_counter();

    /* Draw same textured quad again — must TEXFLUSH (page re-uploaded) */
    EMIT_GP0(0x2C808080);
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(0 | (0 << 8) | (clut_w << 16));
    EMIT_GP0(50 | (10 << 16));
    EMIT_GP0(32 | (0 << 8) | (tp_w << 16));
    EMIT_GP0(10 | (50 << 16));
    EMIT_GP0(0 | (32 << 8));
    EMIT_GP0(50 | (50 << 16));
    EMIT_GP0(32 | (32 << 8));

    gp_gif_scan();
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);

    END_GPU_TEST();
}

/* ================================================================
 *  V10: FillRect zero-size — no-op (PSX hardware behavior)
 * ================================================================ */
static void test_fillrect_zero_size(void)
{
    BEGIN_GPU_TEST("fill_zero");

    /* Pre-fill with non-zero */
    psx_vram_shadow[350 * 1024 + 0] = 0x5555;

    /* FillRect with w=0 → should be no-op */
    EMIT_GP0(0x02FFFFFF);
    EMIT_GP0(0 | (350 << 16));
    EMIT_GP0(0 | (1 << 16));  /* w=0, h=1 */

    /* Pixel should be unchanged */
    EXPECT_VRAM_PIXEL(0, 350, 0x5555);

    END_GPU_TEST();
}

/* ================================================================
 *  V11: GP1(10h) info query — texture window (type 2)
 * ================================================================ */
static void test_gp1_info_tex_window(void)
{
    BEGIN_GPU_TEST("gp1_texwin");

    /* Set E2: texture window mask_x=0xF, mask_y=0xF, off_x=0, off_y=0
     * Raw E2 value = (mask_x) | (mask_y << 5) | (off_x << 10) | (off_y << 15) */
    uint32_t e2 = 0x0F | (0x0F << 5) | (0 << 10) | (0 << 15);
    EMIT_GP0(0xE2000000 | e2);

    /* Query type 2: texture window */
    EMIT_GP1(0x10000002);

    /* gpu_read should contain the raw E2 value */
    if (gpu_read == e2) {
        printf("    %-16s: gpu_read=0x%x OK\n", gp_ctx.name, (unsigned)gpu_read);
    } else {
        printf("  [FAIL] %-16s: gpu_read=0x%x expected 0x%x\n",
               gp_ctx.name, (unsigned)gpu_read, (unsigned)e2);
        gp_ctx.fail_count++;
    }

    END_GPU_TEST();
}

/* ================================================================
 *  V12: GP1(10h) info query — draw offset (type 5)
 * ================================================================ */
static void test_gp1_info_draw_offset(void)
{
    BEGIN_GPU_TEST("gp1_offset");

    /* Set E5: draw offset x=100, y=50
     * Raw E5 = x | (y << 11) */
    uint32_t e5 = 100 | (50 << 11);
    EMIT_GP0(0xE5000000 | e5);

    /* Query type 5 */
    EMIT_GP1(0x10000005);

    if (gpu_read == e5) {
        printf("    %-16s: gpu_read=0x%x OK\n", gp_ctx.name, (unsigned)gpu_read);
    } else {
        printf("  [FAIL] %-16s: gpu_read=0x%x expected 0x%x\n",
               gp_ctx.name, (unsigned)gpu_read, (unsigned)e5);
        gp_ctx.fail_count++;
    }

    END_GPU_TEST();
}

/* ================================================================
 *  Runner
 * ================================================================ */
void gp_run_vram_tests(void)
{
    printf("\n--- Shadow VRAM & Transfer Tests ---\n");

    test_fillrect_shadow();        /* V1 */
    test_fillrect_red();           /* V2 */
    test_fillrect_white();         /* V3 */
    test_fillrect_black();         /* V4 */
    test_fillrect_dirty();         /* V5 */
    test_cpu_to_vram_shadow();     /* V6 */
    test_vram_copy_basic();        /* V7 */
    test_vram_copy_dirty();        /* V8 */
    test_fill_invalidates_texture(); /* V9 */
    test_fillrect_zero_size();     /* V10 */
    test_gp1_info_tex_window();    /* V11 */
    test_gp1_info_draw_offset();   /* V12 */
}
