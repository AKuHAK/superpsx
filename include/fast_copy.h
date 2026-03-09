/**
 * fast_copy.h — Aligned VRAM bulk-copy primitives for PS2 EE (R5900)
 *
 * Problem: ee-gcc's memcpy() emits unaligned ldl/ldr + sdl/sdr even when
 * src and dst are 16-byte aligned.  That's 68 instructions for 128 bytes
 * (17 × 4 iterations), all using 64-bit unaligned ops.
 *
 * Solution: Inline assembly using R5900-native LQ/SQ (128-bit load/store).
 * 128 bytes in 16 instructions (8 LQ + 8 SQ), 256 bytes in 32 instructions.
 * PREF instructions prefetch the next row's cache lines to hide L1 miss
 * latency on strided VRAM access patterns (~2048-byte stride).
 *
 * Requirements:
 *   - Both src and dst MUST be 16-byte aligned
 *   - next_src is used only for prefetch (can equal src on last row)
 *
 * Usage:
 *   fast_copy_128(gif_ptr, &vram[row * 1024 + px], &vram[(row+1) * 1024 + px]);
 *   fast_copy_256(gif_ptr, &vram[row * 1024 + px], &vram[(row+1) * 1024 + px]);
 */
#ifndef FAST_COPY_H
#define FAST_COPY_H

#include <string.h> /* memcpy fallback */

#ifdef _EE /* ═══ PS2 EE target — native 128-bit LQ/SQ ═══════════════════ */

/**
 * Copy 128 bytes (e.g. one 4BPP texture row = 8 QWs).
 * Prefetches 2 cache lines from next_src while copying.
 */
static inline void fast_copy_128(void *dst, const void *src, const void *next_src)
{
    __asm__ volatile (
        /* Prefetch next row (2 cache lines = 128 bytes) */
        "pref 0,  0(%[ns])\n"
        "pref 0, 64(%[ns])\n"
        /* Load 8 quadwords from source */
        "lq $8,   0(%[s])\n"
        "lq $9,  16(%[s])\n"
        "lq $10, 32(%[s])\n"
        "lq $11, 48(%[s])\n"
        "lq $12, 64(%[s])\n"
        "lq $13, 80(%[s])\n"
        "lq $14, 96(%[s])\n"
        "lq $15,112(%[s])\n"
        /* Store 8 quadwords to destination */
        "sq $8,   0(%[d])\n"
        "sq $9,  16(%[d])\n"
        "sq $10, 32(%[d])\n"
        "sq $11, 48(%[d])\n"
        "sq $12, 64(%[d])\n"
        "sq $13, 80(%[d])\n"
        "sq $14, 96(%[d])\n"
        "sq $15,112(%[d])\n"
        :
        : [d] "r"(dst), [s] "r"(src), [ns] "r"(next_src)
        : "$8","$9","$10","$11","$12","$13","$14","$15","memory"
    );
}

/**
 * Copy 256 bytes (e.g. one 8BPP texture row = 16 QWs).
 * Prefetches 4 cache lines from next_src while copying.
 * Two-pass: first 128 bytes then second 128 bytes (only 8 temp regs).
 */
static inline void fast_copy_256(void *dst, const void *src, const void *next_src)
{
    __asm__ volatile (
        /* Prefetch next row (4 cache lines = 256 bytes) */
        "pref 0,   0(%[ns])\n"
        "pref 0,  64(%[ns])\n"
        "pref 0, 128(%[ns])\n"
        "pref 0, 192(%[ns])\n"
        /* First 128 bytes */
        "lq $8,   0(%[s])\n"
        "lq $9,  16(%[s])\n"
        "lq $10, 32(%[s])\n"
        "lq $11, 48(%[s])\n"
        "lq $12, 64(%[s])\n"
        "lq $13, 80(%[s])\n"
        "lq $14, 96(%[s])\n"
        "lq $15,112(%[s])\n"
        "sq $8,   0(%[d])\n"
        "sq $9,  16(%[d])\n"
        "sq $10, 32(%[d])\n"
        "sq $11, 48(%[d])\n"
        "sq $12, 64(%[d])\n"
        "sq $13, 80(%[d])\n"
        "sq $14, 96(%[d])\n"
        "sq $15,112(%[d])\n"
        /* Second 128 bytes */
        "lq $8, 128(%[s])\n"
        "lq $9, 144(%[s])\n"
        "lq $10,160(%[s])\n"
        "lq $11,176(%[s])\n"
        "lq $12,192(%[s])\n"
        "lq $13,208(%[s])\n"
        "lq $14,224(%[s])\n"
        "lq $15,240(%[s])\n"
        "sq $8, 128(%[d])\n"
        "sq $9, 144(%[d])\n"
        "sq $10,160(%[d])\n"
        "sq $11,176(%[d])\n"
        "sq $12,192(%[d])\n"
        "sq $13,208(%[d])\n"
        "sq $14,224(%[d])\n"
        "sq $15,240(%[d])\n"
        :
        : [d] "r"(dst), [s] "r"(src), [ns] "r"(next_src)
        : "$8","$9","$10","$11","$12","$13","$14","$15","memory"
    );
}

#else /* ═══ Host / test builds — plain memcpy fallback ═══════════════════ */

static inline void fast_copy_128(void *dst, const void *src, const void *next_src)
{
    (void)next_src;
    memcpy(dst, src, 128);
}

static inline void fast_copy_256(void *dst, const void *src, const void *next_src)
{
    (void)next_src;
    memcpy(dst, src, 256);
}

#endif /* _EE */

#endif /* FAST_COPY_H */
