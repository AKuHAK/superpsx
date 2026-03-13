/**
 * gpu_psp_backend.c — PSP GPU backend (GU-based rendering)
 *
 * Implements the GPU_Backend_* HAL interface using sceGu.
 * Also defines all shared GPU state variables (externed in gpu_state.h).
 */
#include "gpu_state.h"
#include "gpu_psp_state.h"
#include "gpu_backend.h"
#include "osd.h"
#include "profiler.h"
#include <pspgu.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <psputils.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>

extern uint8_t *psx_ram;

/* ── Global variable definitions (externed in gpu_state.h) ──────────── */
uint32_t gpu_stat = 0x1C000000;
uint32_t gpu_read = 0;
volatile int gpu_pending_vblank_flush = 0;
uint64_t gpu_estimated_pixels = 0;

int fb_address = 0;
int fb_width = PSP_SCREEN_W;
int fb_height = PSP_SCREEN_H;
int fb_psm = 0;

int draw_offset_x = 0;
int draw_offset_y = 0;
int draw_clip_x1 = 0;
int draw_clip_y1 = 0;
int draw_clip_x2 = 1023;
int draw_clip_y2 = 511;

int disp_range_y1 = 0;
int disp_range_y2 = 240;

int display_start_x = 0;
int display_start_y = 0;

int tex_page_x = 0;
int tex_page_y = 0;
int tex_page_format = 0;
int semi_trans_mode = 0;
int dither_enabled = 0;

uint16_t *psx_vram_shadow = NULL;

int vram_tx_x = 0, vram_tx_y = 0, vram_tx_w = 0, vram_tx_h = 0, vram_tx_pixel = 0;
int vram_read_x = 0, vram_read_y = 0, vram_read_w = 0, vram_read_h = 0;
int vram_read_remaining = 0;
int vram_read_pixel = 0;

int polyline_active = 0;
int polyline_shaded = 0;
int polyline_semi_trans = 0;
uint32_t polyline_prev_color = 0;
uint32_t polyline_next_color = 0;
int16_t polyline_prev_x = 0;
int16_t polyline_prev_y = 0;
int polyline_expect_color = 0;

int tex_flip_x = 0;
int tex_flip_y = 0;

int mask_set_bit = 0;
int mask_check_bit = 0;
uint64_t cached_base_test = 0;

int gp1_allow_2mb = 0;

uint32_t tex_win_mask_x = 0;
uint32_t tex_win_mask_y = 0;
uint32_t tex_win_off_x = 0;
uint32_t tex_win_off_y = 0;

int gpu_cmd_remaining = 0;
uint32_t gpu_cmd_buffer[16];
int gpu_cmd_ptr = 0;
int gpu_transfer_words = 0;
int gpu_transfer_total = 0;

uint32_t vram_gen_counter = 0;
uint64_t gpu_busy_until = 0;
gs_state_t gs_state = {0};
gpu_frame_stats_t gpu_frame_stats = {0};

uint32_t raw_tex_window = 0;
uint32_t raw_draw_area_tl = 0;
uint32_t raw_draw_area_br = 0;
uint32_t raw_draw_offset = 0;

/* GIF stub variables (referenced by shared code but unused on PSP) */
unsigned char gif_packet_buf[2][16];
gif_qword_t *fast_gif_ptr = NULL;
gif_qword_t *gif_buffer_end_safe = NULL;
int current_buffer = 0;

/* PSP-specific state */
int psx_active_width = 320;
int psx_active_height = 240;
void *vram_mirror = (void *)PSP_VRAM_OFFSET;

static unsigned int __attribute__((aligned(16))) display_list[262144];
static void *current_fbp = (void *)PSP_FB0_OFFSET;

/* ── GPU Backend Implementation ─────────────────────────────────── */

void GPU_Backend_Init(void) {
    /* Allocate PSX VRAM shadow (1024×512 × 16bpp = 1MB).
     * 64-byte aligned for DCache writeback + GE texture reads. */
    if (!psx_vram_shadow) {
        psx_vram_shadow = (uint16_t *)memalign(64, 1024 * 512 * sizeof(uint16_t));
        if (psx_vram_shadow)
            memset(psx_vram_shadow, 0, 1024 * 512 * sizeof(uint16_t));
    }

    sceGuInit();
    sceGuStart(GU_DIRECT, display_list);

    sceGuDrawBuffer(GU_PSM_5551, (void *)PSP_FB0_OFFSET, PSP_BUF_W);
    sceGuDispBuffer(PSP_SCREEN_W, PSP_SCREEN_H, (void *)PSP_FB1_OFFSET, PSP_BUF_W);
    sceGuDepthBuffer((void *)PSP_ZBUF_OFFSET, PSP_BUF_W);

    sceGuOffset(2048, 2048);
    sceGuViewport(2048, 2048, PSP_SCREEN_W, PSP_SCREEN_H);
    sceGuDepthRange(0xc350, 0x2710);
    sceGuScissor(0, 0, PSP_SCREEN_W, PSP_SCREEN_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);

    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    /* Start the first display list so GP0 commands arriving before
     * the first VBlank have an active list to draw into. */
    sceGuStart(GU_DIRECT, display_list);
    sceGuClearColor(0xFF000000); /* black */
    sceGuClear(GU_COLOR_BUFFER_BIT);

    current_fbp = (void *)PSP_FB0_OFFSET;
}

void GPU_Backend_Flush(void) {
    sceGuFinish();
    sceGuSync(0, 0);
    sceGuStart(GU_DIRECT, display_list); /* re-open list for further draws */
}

void GPU_Backend_FlushSync(void) {
    sceGuFinish();
    sceGuSync(0, 0);
    sceGuStart(GU_DIRECT, display_list); /* re-open list for further draws */
}

void GPU_Backend_SetupEnvironment(void) {
    sceGuStart(GU_DIRECT, display_list);
}

void GPU_Backend_UpdateDisplay(void) {
    /* All GP0 drawing commands already issued sceGu draw calls via
     * Translate_GP0_to_GS.  Just finish/sync/swap the display list. */
    sceGuFinish();
    sceGuSync(0, 0);
    sceGuSwapBuffers();

    /* Clear back buffer for next frame */
    sceGuStart(GU_DIRECT, display_list);
    sceGuClearColor(0xFF000000); /* black */
    sceGuClear(GU_COLOR_BUFFER_BIT);

    /* Reset per-frame draw stats */
    memset(&gpu_frame_stats, 0, sizeof(gpu_frame_stats));
}

void GPU_Backend_VBlank(void) {
    gpu_stat ^= 0x80000000;   /* Toggle GPUSTAT bit 31 (LCF) — BIOS polls this for VSync */
    GPU_Backend_UpdateDisplay();
    osd_vblank_count++;
}

/* ── VRAM Operations ───────────────────────────────────────────── */

void GPU_Backend_StartVRAMTransfer(int x, int y, int w, int h) {
    vram_tx_x = x;
    vram_tx_y = y;
    vram_tx_w = w;
    vram_tx_h = h;
    vram_tx_pixel = 0;
}

#define VRAM_BASE_PTR ((void *)((uintptr_t)sceGeEdramGetAddr() + PSP_VRAM_OFFSET))

void GPU_Backend_UploadShadowVRAM(int x, int y, int w, int h) {
    if (!psx_vram_shadow || w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        uint16_t *src = &psx_vram_shadow[(y + row) * 1024 + x];
        uint16_t *dst = (uint16_t *)VRAM_BASE_PTR + (y + row) * 1024 + x;
        memcpy(dst, src, w * 2);
    }
}

void GPU_Backend_UploadRegionFast(uint32_t coords, uint32_t dims,
                                  uint32_t *data_ptr, uint32_t word_count) {
    int x = coords & 0xFFFF;
    int y = coords >> 16;
    int w = dims & 0xFFFF;
    int h = dims >> 16;
    if (w <= 0 || h <= 0) return;
    (void)word_count;

    for (int row = 0; row < h; row++) {
        memcpy((uint16_t *)VRAM_BASE_PTR + (y + row) * 1024 + x,
               (uint16_t *)data_ptr + row * w,
               w * 2);
    }
}

void GPU_Backend_VRAMCopy(int sx, int sy, int dx, int dy, int w, int h) {
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        uint16_t *src = (uint16_t *)VRAM_BASE_PTR + (sy + row) * 1024 + sx;
        uint16_t *dst = (uint16_t *)VRAM_BASE_PTR + (dy + row) * 1024 + dx;
        memmove(dst, src, w * 2);
    }
}

void GPU_Backend_VRAMWrite(uint32_t word) {
    if (!psx_vram_shadow) return;

    uint16_t p0 = word & 0xFFFF;
    uint16_t p1 = word >> 16;

    int px0 = vram_tx_x + (vram_tx_pixel % vram_tx_w);
    int py0 = vram_tx_y + (vram_tx_pixel / vram_tx_w);
    if (px0 < 1024 && py0 < 512) psx_vram_shadow[py0 * 1024 + px0] = p0;
    vram_tx_pixel++;

    int px1 = vram_tx_x + (vram_tx_pixel % vram_tx_w);
    int py1 = vram_tx_y + (vram_tx_pixel / vram_tx_w);
    if (px1 < 1024 && py1 < 512) psx_vram_shadow[py1 * 1024 + px1] = p1;
    vram_tx_pixel++;
}

void GPU_Backend_VRAMFlush(void) {
    /* After A0 VRAM transfer completes, upload the written region
     * to the sceGu framebuffer as a textured sprite so it becomes
     * visible.  The data is already in psx_vram_shadow. */
    if (!psx_vram_shadow || vram_tx_w <= 0 || vram_tx_h <= 0) return;

    /* Flush dcache so GE can read the shadow VRAM data */
    int tx = vram_tx_x, ty = vram_tx_y;
    int tw = vram_tx_w, th = vram_tx_h;
    sceKernelDcacheWritebackRange(
        &psx_vram_shadow[ty * 1024 + tx],
        (uint32_t)(tw * 2 + (th - 1) * 1024 * 2));

    /* sceGu texture: stride=1024 (shadow VRAM width), PSM_5551, uncached ptr.
     * Texture dimensions must be power-of-2 for sceGuTexImage.
     * Use 512×512 which covers the max PSX VRAM area. */
    sceGuEnable(GU_TEXTURE_2D);
    sceGuDisable(GU_BLEND);
    sceGuTexMode(GU_PSM_5551, 0, 0, 0);
    sceGuTexImage(0, 512, 512, 1024,
                  (void *)((uintptr_t)psx_vram_shadow | 0x40000000));
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);

    /* Draw sprite mapping VRAM region to corresponding screen position.
     * VRAM coordinates are absolute (not draw-offset relative), so
     * map them relative to the display window. */
    typedef struct { float u, v; uint32_t color; int16_t x, y, z; } TVert;
    TVert *v = (TVert *)sceGuGetMemory(2 * sizeof(TVert));
    int16_t sx0 = (int16_t)((int32_t)(tx - display_start_x) * PSP_SCREEN_W / psx_active_width);
    int16_t sy0 = (int16_t)((int32_t)(ty - display_start_y) * PSP_SCREEN_H / psx_active_height);
    int16_t sx1 = (int16_t)((int32_t)(tx + tw - display_start_x) * PSP_SCREEN_W / psx_active_width);
    int16_t sy1 = (int16_t)((int32_t)(ty + th - display_start_y) * PSP_SCREEN_H / psx_active_height);

    v[0].u = (float)tx;       v[0].v = (float)ty;
    v[0].color = 0xFFFFFFFF;
    v[0].x = sx0; v[0].y = sy0; v[0].z = 0;
    v[1].u = (float)(tx + tw); v[1].v = (float)(ty + th);
    v[1].color = 0xFFFFFFFF;
    v[1].x = sx1; v[1].y = sy1; v[1].z = 0;

    sceGuDrawArray(GU_SPRITES,
        GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
        2, 0, v);

    sceGuDisable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);
}

void GPU_Backend_VRAMReadback(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
    /* TODO: Read from GE EDRAM mirror back to shadow VRAM */
}

/* ── Drawing Environment ───────────────────────────────────────── */

void GPU_Backend_SetScissor(int x1, int y1, int x2, int y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
    /* PSP scissor is in screen coordinates; PSX uses VRAM coordinates.
     * The coordinate transform happens in the primitives layer. */
}

void GPU_Backend_SetDisplayFB(int x, int y) {
    display_start_x = x;
    display_start_y = y;
}

void GPU_Backend_SetResolution(int interlace, int mode) {
    /* Decode PSX display resolution from gpu_stat (set by GP1(08h)).
     * Parameters match PS2's SetGsCrt(interlace, mode, 0):
     *   interlace: 0=progressive, 1=interlaced
     *   mode: 2=NTSC, 3=PAL */
    static const int hres_table[] = {256, 320, 512, 640};
    uint32_t hres1 = (gpu_stat >> 17) & 3;
    uint32_t hres2 = (gpu_stat >> 16) & 1;
    uint32_t vres  = (gpu_stat >> 19) & 1;

    psx_active_width = hres2 ? 368 : hres_table[hres1];
    if (mode == 3) /* PAL */
        psx_active_height = vres ? 512 : 256;
    else /* NTSC */
        psx_active_height = vres ? 480 : 240;
    (void)interlace;
}

void GPU_Backend_SetMaskBit(int set, int check) {
    (void)set; (void)check;
    /* TODO: Implement via alpha test or stencil */
}

void GPU_Backend_ClearVRAM(int clip_x1, int clip_y1, int clip_x2, int clip_y2) {
    (void)clip_x1; (void)clip_y1; (void)clip_x2; (void)clip_y2;
    sceGuClearColor(0);
    sceGuClear(GU_COLOR_BUFFER_BIT);
}

void GPU_Backend_InvalidateState(void) {
    Prim_InvalidateGSState();
}

int GPU_Backend_TryFastPoly(uint32_t *cmd_buffer) {
    (void)cmd_buffer;
    return 0; /* No fast path on PSP yet */
}

/* ── PSP-specific stubs for shared code ──────────────────────────── */

VU0JITCache vu0_jit_cache;
void vu0_prepare_mvmva(R3000CPU *cpu, uint32_t mx_cv) {
    (void)cpu; (void)mx_cv;
}

/* ── GPU Read/Write/DMA (shared interface from superpsx.h) ─────── */

uint32_t GPU_Read(void) {
    if (vram_read_remaining > 0) {
        uint16_t p0 = 0, p1 = 0;
        if (psx_vram_shadow && vram_read_w > 0) {
            int px0 = vram_read_x + (vram_read_pixel % vram_read_w);
            int py0 = vram_read_y + (vram_read_pixel / vram_read_w);
            if (px0 < 1024 && py0 < 512) p0 = psx_vram_shadow[py0 * 1024 + px0];
            vram_read_pixel++;
            int px1 = vram_read_x + (vram_read_pixel % vram_read_w);
            int py1 = vram_read_y + (vram_read_pixel / vram_read_w);
            if (px1 < 1024 && py1 < 512) p1 = psx_vram_shadow[py1 * 1024 + px1];
            vram_read_pixel++;
        }
        vram_read_remaining--;
        if (vram_read_remaining == 0) gpu_stat &= ~0x08000000;
        gpu_read = (uint32_t)p0 | ((uint32_t)p1 << 16);
    }
    return gpu_read;
}

uint32_t GPU_ReadStatus(void) {
    /* Fast-forward cycles when GPU is "busy" — needed for DrawSync loops
     * that poll GPUSTAT in tight loops without advancing time. */
    extern uint64_t global_cycles;
    extern uint64_t scheduler_cached_earliest;
    extern void Scheduler_DispatchEvents(uint64_t now);

    if (global_cycles < gpu_busy_until) {
        global_cycles = gpu_busy_until;
        gpu_busy_until = 0;
        while (global_cycles >= scheduler_cached_earliest)
            Scheduler_DispatchEvents(global_cycles);
    }

    uint32_t final_stat = gpu_stat | 0x14002000;

    /* Bit 25: DMA data request — depends on GP1(04h) direction.
     * Dir=0→0, Dir=1→1 (FIFO ready), Dir=2→same as bit28, Dir=3→same as bit27.
     * BIOS gpu_send_dma polls this before starting GPU DMA. */
    uint32_t dma_dir = (final_stat >> 29) & 3;
    if (dma_dir == 1)
        final_stat |= 0x02000000;
    else if (dma_dir == 2)
        final_stat |= 0x02000000;
    else if (dma_dir == 3 && (final_stat & 0x08000000))
        final_stat |= 0x02000000;

    return final_stat;
}

int GPU_DMA2(uint32_t madr, uint32_t bcr, uint32_t chcr) {
    uint32_t addr = madr & 0x1FFFFC;
    uint32_t sync_mode = (chcr >> 9) & 3;
    uint32_t direction = chcr & 1;

    PROF_PUSH(PROF_GPU_DMA);

    if (sync_mode == 0 || sync_mode == 1) {
        if (direction == 1) { /* CPU → GPU */
            uint32_t words = (sync_mode == 0) ? (bcr & 0xFFFF) : ((bcr & 0xFFFF) * ((bcr >> 16) & 0xFFFF));
            if (words == 0) words = 0x10000;
            PROF_PUSH(PROF_GPU_PRIM);
            GPU_ProcessDmaBlock((uint32_t *)(psx_ram + addr), words);
            PROF_POP(PROF_GPU_PRIM);
        } else { /* GPU → CPU (VRAM Read) */
            uint32_t words = (sync_mode == 0) ? (bcr & 0xFFFF) : ((bcr & 0xFFFF) * ((bcr >> 16) & 0xFFFF));
            if (words == 0) words = 0x10000;
            for (uint32_t i = 0; i < words; i++) {
                uint32_t word = GPU_Read();
                *(uint32_t *)(psx_ram + ((addr + i * 4) & 0x1FFFFC)) = word;
            }
        }
    } else if (sync_mode == 2) { /* Linked List */
        uint32_t max_packets = 20000;
        uint32_t packets = 0;

        while (packets < max_packets) {
            uint32_t header = *(uint32_t *)&psx_ram[addr];
            uint32_t count = header >> 24;
            uint32_t next = header & 0xFFFFFF;

            if (count > 0) {
                PROF_PUSH(PROF_GPU_PRIM);
                GPU_ProcessDmaBlock((uint32_t *)&psx_ram[(addr + 4) & 0x1FFFFC], count);
                PROF_POP(PROF_GPU_PRIM);
            }

            if (next == 0xFFFFFF) break;
            addr = next & 0x1FFFFC;
            packets++;
        }
    }

    PROF_POP(PROF_GPU_DMA);
    return 0;
}

void DumpVRAM(const char *filename) { (void)filename; }

/* ── Texture cache stubs (PSP version) ─────────────────────────── */

int Decode_TexPage_Cached(int tex_format, int tpx, int tpy,
                          int clut_x, int clut_y,
                          int *out_slot_x, int *out_slot_y) {
    (void)tex_format; (void)tpx; (void)tpy;
    (void)clut_x; (void)clut_y;
    if (out_slot_x) *out_slot_x = 0;
    if (out_slot_y) *out_slot_y = 0;
    return 0;
}

uint32_t Tex_Cache_GetPageGen(int tex_format, int tex_page_x, int tex_page_y) {
    (void)tex_format; (void)tex_page_x; (void)tex_page_y;
    return vram_gen_counter;
}

void Tex_Cache_DumpStats(void) {}
void Tex_Cache_ResetStats(void) {}
void Tex_Cache_DirtyRegion(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
