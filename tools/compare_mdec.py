#!/usr/bin/env python3
"""Compare VRAM dump against reference PNG for MDEC 15-bit test."""
import sys
import numpy as np
from PIL import Image

dump = sys.argv[1] if len(sys.argv) > 1 else 'build/shadow_34500.bin'
ref_path = 'tests/mdec/frame/vram-15bit.png'

ref = np.array(Image.open(ref_path))
with open(dump, 'rb') as f:
    raw = f.read()
vram = np.frombuffer(raw, dtype=np.uint16).reshape(512, 1024)

exact = close = total = 0
max_diff = 0
diff_sum = 0
for y in range(240):
    for x in range(320):
        v = int(vram[y, x])
        r_us = (v & 0x1F) << 3
        g_us = ((v >> 5) & 0x1F) << 3
        b_us = ((v >> 10) & 0x1F) << 3
        r_ref = int(ref[y, x, 0])
        g_ref = int(ref[y, x, 1])
        b_ref = int(ref[y, x, 2])
        d = max(abs(r_us - r_ref), abs(g_us - g_ref), abs(b_us - b_ref))
        diff_sum += d
        total += 1
        if d == 0:
            exact += 1
        elif d <= 8:
            close += 1
        if d > max_diff:
            max_diff = d

print("Dump:", dump)
print("Total pixels:", total)
print("Exact match: %d (%.1f%%)" % (exact, 100.0*exact/total))
print("Close (<=8): %d (%.1f%%)" % (close, 100.0*close/total))
print("Far (>8):    %d (%.1f%%)" %
      (total-exact-close, 100.0*(total-exact-close)/total))
print("Mean diff:   %.2f" % (diff_sum/total))
print("Max diff:    %d" % max_diff)

# First macroblock detail
print()
ex = cl = 0
for y in range(16):
    for x in range(16):
        v = int(vram[y, x])
        r_us = (v & 0x1F) << 3
        g_us = ((v >> 5) & 0x1F) << 3
        b_us = ((v >> 10) & 0x1F) << 3
        r_ref = int(ref[y, x, 0])
        g_ref = int(ref[y, x, 1])
        b_ref = int(ref[y, x, 2])
        d = max(abs(r_us - r_ref), abs(g_us - g_ref), abs(b_us - b_ref))
        if d == 0:
            ex += 1
        elif d <= 8:
            cl += 1
print("MB0 (16x16): Exact=%d/256, Close=%d/256, Far=%d/256" %
      (ex, cl, 256-ex-cl))

# Show worst pixels
print()
print("=== Worst 20 pixels ===")
worst = []
for y in range(240):
    for x in range(320):
        v = int(vram[y, x])
        r_us = (v & 0x1F) << 3
        g_us = ((v >> 5) & 0x1F) << 3
        b_us = ((v >> 10) & 0x1F) << 3
        r_ref = int(ref[y, x, 0])
        g_ref = int(ref[y, x, 1])
        b_ref = int(ref[y, x, 2])
        d = max(abs(r_us - r_ref), abs(g_us - g_ref), abs(b_us - b_ref))
        if d > 40:
            worst.append((d, y, x, (r_us, g_us, b_us), (r_ref, g_ref, b_ref)))
worst.sort(reverse=True)
for d, y, x, ours, reff in worst[:20]:
    mb_x = x // 16
    mb_y = y // 16
    print("  [%d,%d] MB(%d,%d) diff=%d ours=%s ref=%s" %
          (y, x, mb_y, mb_x, d, ours, reff))

# Error distribution by macroblock row
print()
print("=== Mean diff by macroblock row ===")
for mb_row in range(15):
    row_sum = 0
    row_n = 0
    for y in range(mb_row*16, min(mb_row*16+16, 240)):
        for x in range(320):
            v = int(vram[y, x])
            r_us = (v & 0x1F) << 3
            g_us = ((v >> 5) & 0x1F) << 3
            b_us = ((v >> 10) & 0x1F) << 3
            r_ref = int(ref[y, x, 0])
            g_ref = int(ref[y, x, 1])
            b_ref = int(ref[y, x, 2])
            d = max(abs(r_us - r_ref), abs(g_us - g_ref), abs(b_us - b_ref))
            row_sum += d
            row_n += 1
    print("  Row %2d: mean=%.2f" % (mb_row, row_sum/row_n))
