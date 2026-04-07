/**
 * gpu_trace.h — GPU command stream ring buffer for offline analysis
 *
 * Records raw GP0 words into a 60-frame ring buffer. On trigger (button
 * combo), dumps the last 60 frames to a binary file for offline analysis
 * with tools/gpu_trace_analyze.py.
 *
 * Compiled only when ENABLE_GPU_TRACE is defined (CMake option).
 */
#ifndef GPU_TRACE_H
#define GPU_TRACE_H

#include <stdint.h>

#ifdef ENABLE_GPU_TRACE

#define GPU_TRACE_FRAMES      60
#define GPU_TRACE_MAX_WORDS   16384  /* per frame (~64KB) */

/* Binary file format:
 *   Header:  magic("GPTD") | version(1) | frame_count | max_words
 *   Per frame: frame_id(u32) | word_count(u32) | gp0_data[word_count]
 */
#define GPU_TRACE_MAGIC   0x44545047  /* "GPTD" little-endian */
#define GPU_TRACE_VERSION 1

void gpu_trace_init(void);
void gpu_trace_record(const uint32_t *data, uint32_t word_count);
void gpu_trace_frame_end(void);
void gpu_trace_trigger_dump(const char *path);

#else

static inline void gpu_trace_init(void) {}
static inline void gpu_trace_record(const uint32_t *d, uint32_t n) { (void)d; (void)n; }
static inline void gpu_trace_frame_end(void) {}
static inline void gpu_trace_trigger_dump(const char *p) { (void)p; }

#endif /* ENABLE_GPU_TRACE */
#endif /* GPU_TRACE_H */
