/**
 * gpu_trace.c — GPU command stream ring buffer implementation
 *
 * Records raw GP0 DMA words into a 60-frame circular buffer.
 * On trigger, writes the buffer to a binary file (oldest frame first).
 *
 * Memory cost: ~3.8 MB BSS (60 × 16384 × 4 bytes).
 */
#include "gpu_trace.h"

#ifdef ENABLE_GPU_TRACE

#include <stdio.h>
#include <string.h>

/* ── Ring buffer storage ───────────────────────────────────────── */
static uint32_t trace_buf[GPU_TRACE_FRAMES][GPU_TRACE_MAX_WORDS];
static uint32_t trace_count[GPU_TRACE_FRAMES];    /* words used per slot */
static uint32_t trace_frame_id[GPU_TRACE_FRAMES]; /* global frame number */
static int      trace_pos = 0;                    /* current write slot */
static uint32_t trace_global_frame = 0;
static int      trace_dump_pending = 0;
static const char *trace_dump_path = NULL;

void gpu_trace_init(void)
{
    memset(trace_count, 0, sizeof(trace_count));
    trace_pos = 0;
    trace_global_frame = 0;
    trace_dump_pending = 0;
    trace_dump_path = NULL;
}

void gpu_trace_record(const uint32_t *data, uint32_t word_count)
{
    uint32_t pos = trace_pos;
    uint32_t cur = trace_count[pos];
    uint32_t avail = GPU_TRACE_MAX_WORDS - cur;
    uint32_t n = word_count < avail ? word_count : avail;
    if (n > 0) {
        memcpy(&trace_buf[pos][cur], data, n * sizeof(uint32_t));
        trace_count[pos] = cur + n;
    }
}

void gpu_trace_frame_end(void)
{
    /* Finalize current frame */
    trace_frame_id[trace_pos] = trace_global_frame++;
    trace_pos = (trace_pos + 1) % GPU_TRACE_FRAMES;
    trace_count[trace_pos] = 0; /* clear next slot for new frame */

    /* Deferred dump: write after frame boundary so we capture complete frames */
    if (trace_dump_pending && trace_dump_path) {
        trace_dump_pending = 0;

        FILE *f = fopen(trace_dump_path, "wb");
        if (!f) {
            printf("[GPU_TRACE] cannot open %s\n", trace_dump_path);
            return;
        }

        /* Write header */
        uint32_t header[4];
        header[0] = GPU_TRACE_MAGIC;
        header[1] = GPU_TRACE_VERSION;
        header[2] = GPU_TRACE_FRAMES;
        header[3] = GPU_TRACE_MAX_WORDS;
        fwrite(header, sizeof(uint32_t), 4, f);

        /* Write frames oldest-first (slot after trace_pos is oldest) */
        for (int i = 0; i < GPU_TRACE_FRAMES; i++) {
            int idx = (trace_pos + i) % GPU_TRACE_FRAMES;
            fwrite(&trace_frame_id[idx], sizeof(uint32_t), 1, f);
            fwrite(&trace_count[idx], sizeof(uint32_t), 1, f);
            if (trace_count[idx] > 0) {
                fwrite(trace_buf[idx], sizeof(uint32_t), trace_count[idx], f);
            }
        }

        fclose(f);
        printf("[GPU_TRACE] dumped %d frames (%lu-%lu) to %s\n",
               GPU_TRACE_FRAMES,
               (unsigned long)trace_frame_id[(trace_pos) % GPU_TRACE_FRAMES],
               (unsigned long)trace_frame_id[(trace_pos + GPU_TRACE_FRAMES - 1) % GPU_TRACE_FRAMES],
               trace_dump_path);
    }
}

void gpu_trace_trigger_dump(const char *path)
{
    trace_dump_path = path;
    trace_dump_pending = 1;
    printf("[GPU_TRACE] dump requested → %s\n", path);
}

#endif /* ENABLE_GPU_TRACE */
