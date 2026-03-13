/**
 * gpu_psp_primitives.c — PSX GP0 primitive → PSP GU translation
 */
#include "gpu_state.h"
#include "gpu_psp_state.h"
#include "profiler.h"
#include <pspgu.h>
#include <string.h>

extern uint64_t gpu_estimated_pixels;

/* ── Coordinate Transformation ──────────────────────────────────── */
/* PSX_to_PSP is now in gpu_psp_state.h */

static inline uint32_t PSX_to_ABGR(uint32_t c)
{
    return (c & 0x00FFFFFF) | 0xFF000000;
}

/* ── State Management ───────────────────────────────────────────── */

static void apply_blend(int is_semi_trans)
{
    if (is_semi_trans)
    {
        sceGuEnable(GU_BLEND);
        switch (semi_trans_mode)
        {
        case 0:
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
            break;
        case 1:
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFFFF, 0xFFFFFFFF);
            break;
        case 2:
            sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0xFFFFFFFF, 0xFFFFFFFF);
            break;
        case 3:
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x40404040, 0xFFFFFFFF);
            break;
        }
    }
    else
    {
        sceGuDisable(GU_BLEND);
    }
}

void Prim_InvalidateGSState(void)
{
    gs_state.valid = 0;
}

void Prim_InvalidateTexCache(void) {}
void Prim_InvalidateTexCache_Page(int tpx, int tpy)
{
    (void)tpx;
    (void)tpy;
}

/* ── Primary Dispatch ───────────────────────────────────────────── */

int GPU_TryFastEmit(uint32_t *psx_cmd)
{
    (void)psx_cmd;
    return 0;
}

int Translate_GP0_to_GS(uint32_t *psx_cmd)
{
    uint32_t cmd_word = psx_cmd[0];
    uint32_t cmd = (cmd_word >> 24) & 0xFF;

    /* FillRect (GP0 0x02) — special case before the switch */
    if (cmd == 0x02)
    {
        uint32_t color = PSX_to_ABGR(psx_cmd[0]);
        uint32_t xy = psx_cmd[1];
        uint32_t wh = psx_cmd[2];
        int16_t fx = (int16_t)(xy & 0x3F0); /* X aligned to 16px */
        int16_t fy = (int16_t)((xy >> 16) & 0x1FF);
        int16_t fw = (int16_t)(((wh & 0x3FF) + 0xF) & ~0xF); /* round up to 16 */
        int16_t fh = (int16_t)((wh >> 16) & 0x1FF);
        if (fw == 0 || fh == 0)
            return 3;

        /* Update psx_vram_shadow for CPU readback / texture use */
        if (psx_vram_shadow)
        {
            uint16_t r5 = (uint16_t)((cmd_word >> 3) & 0x1F);
            uint16_t g5 = (uint16_t)((cmd_word >> 11) & 0x1F);
            uint16_t b5 = (uint16_t)((cmd_word >> 19) & 0x1F);
            uint16_t col16 = r5 | (g5 << 5) | (b5 << 10);
            for (int y = fy; y < fy + fh && y < 512; y++)
                for (int x = fx; x < fx + fw && x < 1024; x++)
                    psx_vram_shadow[y * 1024 + x] = col16;
        }

        /* Draw via sceGu — FillRect uses absolute VRAM coords, not draw-offset */
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_BLEND);
        int16_t sx0 = (int16_t)((int32_t)(fx - display_start_x) * PSP_SCREEN_W / psx_active_width);
        int16_t sy0 = (int16_t)((int32_t)(fy - display_start_y) * PSP_SCREEN_H / psx_active_height);
        int16_t sx1 = (int16_t)((int32_t)(fx + fw - display_start_x) * PSP_SCREEN_W / psx_active_width);
        int16_t sy1 = (int16_t)((int32_t)(fy + fh - display_start_y) * PSP_SCREEN_H / psx_active_height);
        PspVertFlat *v = (PspVertFlat *)sceGuGetMemory(2 * sizeof(PspVertFlat));
        v[0].color = color;
        v[0].x = sx0;
        v[0].y = sy0;
        v[0].z = 0;
        v[1].color = color;
        v[1].x = sx1;
        v[1].y = sy1;
        v[1].z = 0;
        sceGuDrawArray(GU_SPRITES,
                       GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                       2, NULL, v);
        gpu_frame_stats.fill++;
        return 3;
    }

    switch (cmd & 0xE0)
    {
    case 0x20:
    { /* Polygons */
        int is_quad = (cmd & 0x08) != 0;
        int is_shaded = (cmd & 0x10) != 0;
        int is_textured = (cmd & 0x04) != 0;
        int is_semi = (cmd & 0x02) != 0;
        int nv = is_quad ? 4 : 3;

        apply_blend(is_semi);

        if (!is_textured)
        {
            sceGuDisable(GU_TEXTURE_2D);
            PspVertFlat *v = (PspVertFlat *)sceGuGetMemory(nv * sizeof(PspVertFlat));
            uint32_t base_color = PSX_to_ABGR(psx_cmd[0]);
            int p = 1; /* skip cmd+color word */
            for (int i = 0; i < nv; i++)
            {
                uint32_t color;
                if (i == 0)
                {
                    color = base_color;
                }
                else if (is_shaded)
                {
                    color = PSX_to_ABGR(psx_cmd[p++]);
                }
                else
                {
                    color = base_color;
                }
                int16_t x, y;
                PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x, &y);
                p++;
                v[i].color = color;
                v[i].x = x;
                v[i].y = y;
                v[i].z = 0;
            }
            sceGuDrawArray(is_quad ? GU_TRIANGLE_STRIP : GU_TRIANGLES,
                           GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                           nv, NULL, v);
            gpu_frame_stats.poly_flat++;
            return p;
        }
        else
        {
            sceGuEnable(GU_TEXTURE_2D);
            PspVertTex *v = (PspVertTex *)sceGuGetMemory(nv * sizeof(PspVertTex));
            uint32_t base_color = PSX_to_ABGR(psx_cmd[0]);
            int p = 1; /* skip cmd+color word */
            for (int i = 0; i < nv; i++)
            {
                uint32_t color;
                if (i == 0)
                {
                    color = base_color;
                }
                else if (is_shaded)
                {
                    color = PSX_to_ABGR(psx_cmd[p++]);
                }
                else
                {
                    color = base_color;
                }
                int16_t x, y;
                PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x, &y);
                p++;
                uint32_t uv_clut = psx_cmd[p++];
                v[i].u = (float)(uv_clut & 0xFF);
                v[i].v = (float)((uv_clut >> 8) & 0xFF);
                v[i].color = color;
                v[i].x = x;
                v[i].y = y;
                v[i].z = 0;
            }
            sceGuDrawArray(is_quad ? GU_TRIANGLE_STRIP : GU_TRIANGLES,
                           GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                           nv, NULL, v);
            gpu_frame_stats.poly_tex++;
            return p;
        }
    }

    case 0x40:
    { /* Lines */
        int is_semi = (cmd & 0x02) != 0;
        int is_shaded = (cmd & 0x10) != 0;
        sceGuDisable(GU_TEXTURE_2D);
        apply_blend(is_semi);

        PspVertFlat *v = (PspVertFlat *)sceGuGetMemory(2 * sizeof(PspVertFlat));
        uint32_t c0 = PSX_to_ABGR(psx_cmd[0]);
        int p = 1;
        int16_t x0, y0, x1, y1;
        PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x0, &y0);
        p++;
        uint32_t c1 = is_shaded ? PSX_to_ABGR(psx_cmd[p++]) : c0;
        PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x1, &y1);
        p++;
        v[0].color = c0;
        v[0].x = x0;
        v[0].y = y0;
        v[0].z = 0;
        v[1].color = c1;
        v[1].x = x1;
        v[1].y = y1;
        v[1].z = 0;
        sceGuDrawArray(GU_LINES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                       2, NULL, v);
        gpu_frame_stats.line++;
        return p;
    }

    case 0x60:
    { /* Rectangles / Sprites */
        int is_textured = (cmd & 0x04) != 0;
        int is_semi = (cmd & 0x02) != 0;
        uint32_t color = PSX_to_ABGR(psx_cmd[0]);

        int16_t x0, y0;
        int p = 1;
        PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x0, &y0);
        p++;

        if (is_textured)
            p++; /* Skip texcoord+clut */

        int16_t w, h;
        int size_code = (cmd >> 3) & 3;
        if (size_code == 0)
        {
            int wp = is_textured ? p : p;
            w = (int16_t)(psx_cmd[wp] & 0xFFFF);
            h = (int16_t)(psx_cmd[wp] >> 16);
            p++;
        }
        else
        {
            static const int16_t sz[] = {0, 1, 8, 16};
            w = sz[size_code];
            h = sz[size_code];
        }

        apply_blend(is_semi);
        if (!is_textured)
            sceGuDisable(GU_TEXTURE_2D);
        else
            sceGuEnable(GU_TEXTURE_2D);

        PspVertFlat *v = (PspVertFlat *)sceGuGetMemory(2 * sizeof(PspVertFlat));
        v[0].color = color;
        v[0].x = x0;
        v[0].y = y0;
        v[0].z = 0;
        v[1].color = color;
        v[1].x = (int16_t)(x0 + w);
        v[1].y = (int16_t)(y0 + h);
        v[1].z = 0;
        sceGuDrawArray(GU_SPRITES,
                       GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                       2, NULL, v);
        gpu_frame_stats.rect_flat++;
        return p;
    }

    default:
        return 0;
    }
}

void Emit_Line_Segment_AD(int16_t x0, int16_t y0, uint32_t color0,
                          int16_t x1, int16_t y1, uint32_t color1,
                          int is_shaded, int is_semi_trans)
{
    sceGuDisable(GU_TEXTURE_2D);
    apply_blend(is_semi_trans);
    PspVertFlat *v = (PspVertFlat *)sceGuGetMemory(2 * sizeof(PspVertFlat));
    int16_t sx0, sy0, sx1, sy1;
    PSX_to_PSP(x0, y0, &sx0, &sy0);
    PSX_to_PSP(x1, y1, &sx1, &sy1);
    v[0].color = PSX_to_ABGR(color0);
    v[0].x = sx0;
    v[0].y = sy0;
    v[0].z = 0;
    v[1].color = is_shaded ? PSX_to_ABGR(color1) : PSX_to_ABGR(color0);
    v[1].x = sx1;
    v[1].y = sy1;
    v[1].z = 0;
    sceGuDrawArray(GU_LINES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, NULL, v);
}
