#!/usr/bin/env python3
"""Compare VRAM dump pixels against reference PNG pixel by pixel."""
import sys
from PIL import Image
import numpy as np

ref = np.array(Image.open('tests/mdec/frame/vram-15bit.png'))
print('Reference PNG shape:', ref.shape, 'dtype:', ref.dtype)

with open('build/shadow_3500.bin', 'rb') as f:
    raw = f.read()
vram = np.frombuffer(raw, dtype=np.uint16).reshape(512, 1024)

print()
print('Row 0, first 32 pixels:')
print('{:>6} | {:>20} | {:>20} | {:>10}'.format(
    'Pixel', 'Our VRAM', 'Reference', 'Diff'))
for x in range(32):
    v = int(vram[0, x])
    r_us = (v & 0x1F) << 3
    g_us = ((v >> 5) & 0x1F) << 3
    b_us = ((v >> 10) & 0x1F) << 3
    r_ref, g_ref, b_ref = int(ref[0, x, 0]), int(
        ref[0, x, 1]), int(ref[0, x, 2])
    dr, dg, db = abs(r_us-r_ref), abs(g_us-g_ref), abs(b_us-b_ref)
    match = 'OK' if (dr+dg+db) == 0 else 'd={}'.format(max(dr, dg, db))
    print('{:6d} | ({:3d},{:3d},{:3d}) | ({:3d},{:3d},{:3d}) | {}'.format(
        x, r_us, g_us, b_us, r_ref, g_ref, b_ref, match))

# Also check rows 8 and 16 (macroblock boundaries)
for row in [8, 16]:
    print()
    print('Row {}, first 32 pixels:'.format(row))
    for x in range(32):
        v = int(vram[row, x])
        r_us = (v & 0x1F) << 3
        g_us = ((v >> 5) & 0x1F) << 3
        b_us = ((v >> 10) & 0x1F) << 3
        r_ref, g_ref, b_ref = int(ref[row, x, 0]), int(
            ref[row, x, 1]), int(ref[row, x, 2])
        dr, dg, db = abs(r_us-r_ref), abs(g_us-g_ref), abs(b_us-b_ref)
        match = 'OK' if (dr+dg+db) == 0 else 'd={}'.format(max(dr, dg, db))
        print('{:6d} | ({:3d},{:3d},{:3d}) | ({:3d},{:3d},{:3d}) | {}'.format(
            x, r_us, g_us, b_us, r_ref, g_ref, b_ref, match))

# Count how many of the first 16x16 block match per-pixel
print()
print('=== First macroblock (16x16) match analysis ===')
exact = 0
close = 0  # within 8
total = 256
for y in range(16):
    for x in range(16):
        v = int(vram[y, x])
        r_us = (v & 0x1F) << 3
        g_us = ((v >> 5) & 0x1F) << 3
        b_us = ((v >> 10) & 0x1F) << 3
        r_ref, g_ref, b_ref = int(ref[y, x, 0]), int(
            ref[y, x, 1]), int(ref[y, x, 2])
        dr, dg, db = abs(r_us-r_ref), abs(g_us-g_ref), abs(b_us-b_ref)
        if max(dr, dg, db) == 0:
            exact += 1
        elif max(dr, dg, db) <= 8:
            close += 1
print('Exact: {}/{}, Close(<=8): {}/{}, Far: {}/{}'.format(
    exact, total, close, total, total-exact-close, total))

# Show cross-macroblock boundary check
# MB0 is 0-15, MB1 is 16-31 in first tile row
print()
print('=== Boundary check: MB0 last col (x=15) vs MB1 first col (x=16) ===')
for y in range(4):
    for x in [15, 16]:
        v = int(vram[y, x])
        r_us = (v & 0x1F) << 3
        g_us = ((v >> 5) & 0x1F) << 3
        b_us = ((v >> 10) & 0x1F) << 3
        r_ref, g_ref, b_ref = int(ref[y, x, 0]), int(
            ref[y, x, 1]), int(ref[y, x, 2])
        dr, dg, db = abs(r_us-r_ref), abs(g_us-g_ref), abs(b_us-b_ref)
        tag = 'MB0-end' if x == 15 else 'MB1-start'
        print('  [{},{}] {} ours=({:3d},{:3d},{:3d}) ref=({:3d},{:3d},{:3d}) diff=({},{},{})'.format(
            y, x, tag, r_us, g_us, b_us, r_ref, g_ref, b_ref, dr, dg, db))

# Raw 16-bit value analysis
print()
print('=== Raw 16-bit values: our VRAM vs reference (reconstructed) ===')
print('Row 0, first 8:')
for x in range(8):
    v_us = int(vram[0, x])
    r_ref, g_ref, b_ref = int(ref[0, x, 0]), int(
        ref[0, x, 1]), int(ref[0, x, 2])
    v_ref = (r_ref >> 3) | ((g_ref >> 3) << 5) | ((b_ref >> 3) << 10)
    print('  [{}] ours=0x{:04x}  ref=0x{:04x}  diff_bits=0x{:04x}'.format(
        x, v_us, v_ref, v_us ^ v_ref))
