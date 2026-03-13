#!/usr/bin/env python3
"""Analyze MB0 Y4 block and MB1 from DMA1 raw output."""
from PIL import Image
import numpy as np
with open('build/mdec_dma1_0.bin', 'rb') as f:
    raw = f.read()
mb0 = np.frombuffer(raw[:512], dtype=np.uint16).reshape(16, 16)


def rgb(v):
    return ((v & 0x1F) << 3, ((v >> 5) & 0x1F) << 3, ((v >> 10) & 0x1F) << 3)


print('MB0 Y4-area (rows 8-15, cols 8-15):')
for y in range(8, 16):
    vals = []
    for x in range(8, 16):
        r, g, b = rgb(int(mb0[y, x]))
        vals.append('({:3d},{:3d},{:3d})'.format(r, g, b))
    print('  row {:2d}: {}'.format(y, ' '.join(vals)))

mb1 = np.frombuffer(raw[512:1024], dtype=np.uint16).reshape(16, 16)
print()
print('MB1 all 16 rows, first 4 pixels each:')
for y in range(16):
    vals = []
    for x in range(4):
        r, g, b = rgb(int(mb1[y, x]))
        vals.append('({:3d},{:3d},{:3d})'.format(r, g, b))
    print('  row {:2d}: {}'.format(y, ' '.join(vals)))

# Check if MB1 has any non-gray pixels
non_gray = 0
total = 256
for y in range(16):
    for x in range(16):
        r, g, b = rgb(int(mb1[y, x]))
        if r != 128 or g != 128 or b != 128:
            non_gray += 1
print()
print('MB1: {}/{} non-gray pixels'.format(non_gray, total))

# Look at reference for comparison
ref = np.array(Image.open('tests/mdec/frame/vram-15bit.png'))
print()
print('Reference at MB1 location (cols 16-31):')
for y in range(4):
    vals = []
    for x in range(16, 20):
        vals.append('({:3d},{:3d},{:3d})'.format(
            int(ref[y, x, 0]), int(ref[y, x, 1]), int(ref[y, x, 2])))
    print('  row {:2d}: {}'.format(y, ' '.join(vals)))
