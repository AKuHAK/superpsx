#!/usr/bin/env python3
"""Analyze MDEC 15-bit VRAM dump vs reference."""
from PIL import Image
import numpy as np
import sys

dump_path = sys.argv[1] if len(sys.argv) > 1 else 'build/shadow_3500.bin'
ref_path = sys.argv[2] if len(
    sys.argv) > 2 else 'tests/mdec/frame/vram-15bit.png'

with open(dump_path, 'rb') as f:
    data = f.read()
pixels = np.frombuffer(data, dtype=np.uint16).reshape((512, 1024))
r = ((pixels & 0x001F) << 3).astype(np.uint8)
g = (((pixels >> 5) & 0x001F) << 3).astype(np.uint8)
b = (((pixels >> 10) & 0x001F) << 3).astype(np.uint8)
dump = np.stack([r, g, b], axis=2)

ref = np.array(Image.open(ref_path).convert('RGB'))

# Display area
d = dump[:240, :320]
r_area = ref[:240, :320]
diff = np.abs(d.astype(int) - r_area.astype(int))

print(f'Display area (320x240):')
print(f'  Mean diff: {diff.mean():.2f}')
print(f'  Max diff: {diff.max()}')
exact = np.sum(diff.sum(axis=2) == 0)
total = 240 * 320
print(f'  Exact pixels: {exact}/{total} ({100*exact/total:.1f}%)')

print()
print('First 16 pixels (row 0):')
for x in range(16):
    dp = dump[0, x]
    rp = ref[0, x]
    raw = int(pixels[0, x])
    print(f'  [{x:2d}] raw={raw:#06x} dump=({dp[0]:3d},{dp[1]:3d},{dp[2]:3d}) ref=({rp[0]:3d},{rp[1]:3d},{rp[2]:3d}) diff=({abs(int(dp[0])-int(rp[0])):3d},{abs(int(dp[1])-int(rp[1])):3d},{abs(int(dp[2])-int(rp[2])):3d})')

print()
print('Row 120 (middle), cols 0-15:')
for x in range(16):
    dp = dump[120, x]
    rp = ref[120, x]
    raw = int(pixels[120, x])
    print(f'  [{x:2d}] raw={raw:#06x} dump=({dp[0]:3d},{dp[1]:3d},{dp[2]:3d}) ref=({rp[0]:3d},{rp[1]:3d},{rp[2]:3d}) diff=({abs(int(dp[0])-int(rp[0])):3d},{abs(int(dp[1])-int(rp[1])):3d},{abs(int(dp[2])-int(rp[2])):3d})')

# Distribution of diff magnitudes
per_pixel_max = diff.max(axis=2)
print()
print('Diff distribution (per-pixel max channel diff):')
for threshold in [0, 8, 16, 24, 32, 48, 64, 128, 248]:
    count = np.sum(per_pixel_max == threshold)
    if count > 0:
        print(f'  diff={threshold:3d}: {count} pixels')

# Check ref quantization
ref_mod8 = ref[:240, :320] % 8
print()
print(
    f'Ref pixels with all channels mult of 8: {np.sum(np.all(ref_mod8 == 0, axis=2))}/{total}')

# Where are the big diffs?
big_locs = np.where(per_pixel_max >= 64)
if len(big_locs[0]) > 0:
    print(f'\nLarge diffs (>=64), first 10:')
    for i in range(min(10, len(big_locs[0]))):
        y, x = big_locs[0][i], big_locs[1][i]
        dp = dump[y, x]
        rp = ref[y, x]
        print(
            f'  ({x},{y}): dump=({dp[0]:3d},{dp[1]:3d},{dp[2]:3d}) ref=({rp[0]:3d},{rp[1]:3d},{rp[2]:3d})')
