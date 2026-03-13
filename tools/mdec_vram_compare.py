#!/usr/bin/env python3
"""Generate PNG from VRAM dump and compare against MDEC 15-bit reference."""
import sys
import numpy as np
from PIL import Image

dump = sys.argv[1] if len(sys.argv) > 1 else 'build/shadow_28500.bin'
ref_path = 'tests/mdec/frame/vram-15bit.png'

with open(dump, 'rb') as f:
    raw = f.read()
vram = np.frombuffer(raw, dtype=np.uint16).reshape(512, 1024)

r = ((vram & 0x001F) << 3).astype(np.uint8)
g = (((vram >> 5) & 0x001F) << 3).astype(np.uint8)
b = (((vram >> 10) & 0x001F) << 3).astype(np.uint8)
rgb = np.stack([r, g, b], axis=2)

img_full = Image.fromarray(rgb, 'RGB')
img_full.save('build/vram_current.png')
print('Saved build/vram_current.png (1024x512)')

img_crop = img_full.crop((0, 0, 320, 240))
img_crop.save('build/vram_mdec_320x240.png')
print('Saved build/vram_mdec_320x240.png (320x240)')

ref = np.array(Image.open(ref_path))
print('Reference shape:', ref.shape)

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

print()
print('=== Pixel comparison (320x240) ===')
print('Total pixels:', total)
print('Exact match: %d (%.1f%%)' % (exact, 100.0 * exact / total))
print('Close (<=8): %d (%.1f%%)' % (close, 100.0 * close / total))
print('Far (>8):    %d (%.1f%%)' %
      (total - exact - close, 100.0 * (total - exact - close) / total))
print('Mean diff:   %.2f' % (diff_sum / total))
print('Max diff:    %d' % max_diff)

print()
print('=== Mean diff by MB row ===')
for mb_row in range(15):
    row_sum = 0
    row_n = 0
    for y in range(mb_row * 16, min(mb_row * 16 + 16, 240)):
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
    print('  Row %2d: mean=%.2f' % (mb_row, row_sum / row_n))

# Generate diff image (amplified 4x for visibility)
diff_img = np.zeros((240, 320, 3), dtype=np.uint8)
for y in range(240):
    for x in range(320):
        v = int(vram[y, x])
        r_us = (v & 0x1F) << 3
        g_us = ((v >> 5) & 0x1F) << 3
        b_us = ((v >> 10) & 0x1F) << 3
        r_ref = int(ref[y, x, 0])
        g_ref = int(ref[y, x, 1])
        b_ref = int(ref[y, x, 2])
        diff_img[y, x, 0] = min(255, abs(r_us - r_ref) * 4)
        diff_img[y, x, 1] = min(255, abs(g_us - g_ref) * 4)
        diff_img[y, x, 2] = min(255, abs(b_us - b_ref) * 4)

Image.fromarray(diff_img).save('build/vram_diff_4x.png')
print()
print('Saved build/vram_diff_4x.png (diff amplified 4x)')
