#!/usr/bin/env python3
"""Compare 4BPP/8BPP MDEC test output against reference PNG."""
import sys
import numpy as np
from PIL import Image

if len(sys.argv) < 3:
    print("Usage: compare_clut.py <ref.png> <shadow.bin>")
    sys.exit(1)

ref_path = sys.argv[1]
dump_path = sys.argv[2]

ref = np.array(Image.open(ref_path))
with open(dump_path, 'rb') as f:
    raw = f.read()
vram = np.frombuffer(raw, dtype=np.uint16).reshape(512, 1024)

# Convert VRAM 15-bit to 8-bit RGB
r = ((vram & 0x001F) << 3).astype(np.uint8)
g = (((vram >> 5) & 0x001F) << 3).astype(np.uint8)
b = (((vram >> 10) & 0x001F) << 3).astype(np.uint8)
vram_rgb = np.stack([r, g, b], axis=2)

# Save our VRAM as PNG (cropped to ref size)
h, w = ref.shape[:2]
crop = Image.fromarray(vram_rgb[:h, :w], 'RGB')
crop.save('build/clut_current.png')
print("Saved build/clut_current.png (%dx%d)" % (w, h))

# Find non-zero region in reference
nz_mask = ref.max(axis=2) > 0
if not nz_mask.any():
    print("Reference is all black!")
    sys.exit(0)

nz_y, nz_x = np.where(nz_mask)
y0, y1 = nz_y.min(), nz_y.max()
x0, x1 = nz_x.min(), nz_x.max()
print("Ref non-zero region: y=%d-%d x=%d-%d" % (y0, y1, x0, x1))

# Compare in the non-zero region
exact = close = total = 0
diff_sum = 0
max_diff = 0
for y in range(y0, y1 + 1):
    for x in range(x0, x1 + 1):
        r_ref, g_ref, b_ref = int(ref[y, x, 0]), int(
            ref[y, x, 1]), int(ref[y, x, 2])
        r_us, g_us, b_us = int(vram_rgb[y, x, 0]), int(
            vram_rgb[y, x, 1]), int(vram_rgb[y, x, 2])
        d = max(abs(r_us - r_ref), abs(g_us - g_ref), abs(b_us - b_ref))
        diff_sum += d
        total += 1
        if d == 0:
            exact += 1
        elif d <= 8:
            close += 1
        if d > max_diff:
            max_diff = d

print()
print("=== Comparison (non-zero region) ===")
print("Total pixels: %d" % total)
print("Exact match: %d (%.1f%%)" % (exact, 100.0 * exact / total))
print("Close (<=8): %d (%.1f%%)" % (close, 100.0 * close / total))
print("Far (>8):    %d (%.1f%%)" %
      (total - exact - close, 100.0 * (total - exact - close) / total))
print("Mean diff:   %.2f" % (diff_sum / total))
print("Max diff:    %d" % max_diff)

# Show first row samples
print()
print("=== First non-zero row (y=%d) ===" % y0)
for x in range(x0, min(x0 + 16, x1 + 1)):
    r_ref, g_ref, b_ref = int(ref[y0, x, 0]), int(
        ref[y0, x, 1]), int(ref[y0, x, 2])
    r_us, g_us, b_us = int(vram_rgb[y0, x, 0]), int(
        vram_rgb[y0, x, 1]), int(vram_rgb[y0, x, 2])
    d = max(abs(r_us - r_ref), abs(g_us - g_ref), abs(b_us - b_ref))
    print("  [%d,%d] ref=(%3d,%3d,%3d) ours=(%3d,%3d,%3d) diff=%d" %
          (y0, x, r_ref, g_ref, b_ref, r_us, g_us, b_us, d))
