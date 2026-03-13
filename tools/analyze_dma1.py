#!/usr/bin/env python3
"""Analyze raw DMA1 output (macroblock-order) vs shadow VRAM pixels."""
import numpy as np
import sys

# Load DMA1 raw dump (macroblock-order 15-bit pixels)
dma_path = sys.argv[1] if len(sys.argv) > 1 else 'build/mdec_dma1_0.bin'
shadow_path = sys.argv[2] if len(sys.argv) > 2 else 'build/shadow_3500.bin'

with open(dma_path, 'rb') as f:
    dma = np.frombuffer(f.read(), dtype=np.uint16)
print(f'DMA1 dump: {len(dma)} pixels ({len(dma)*2} bytes)')
print(f'  = {len(dma)//256} macroblocks of 16x16')

# Print first 32 pixels (2 rows of first macroblock)
print('\nFirst macroblock, row 0 (pixels 0-15):')
for i in range(16):
    v = int(dma[i])
    r = (v & 0x1F) << 3
    g = ((v >> 5) & 0x1F) << 3
    b = ((v >> 10) & 0x1F) << 3
    print(f'  [{i:2d}] {v:#06x} -> ({r:3d},{g:3d},{b:3d})')

print('\nFirst macroblock, row 1 (pixels 16-31):')
for i in range(16, 32):
    v = int(dma[i])
    r = (v & 0x1F) << 3
    g = ((v >> 5) & 0x1F) << 3
    b = ((v >> 10) & 0x1F) << 3
    print(f'  [{i:2d}] {v:#06x} -> ({r:3d},{g:3d},{b:3d})')

# Load shadow VRAM for comparison
with open(shadow_path, 'rb') as f:
    shadow = np.frombuffer(f.read(), dtype=np.uint16).reshape((512, 1024))

# Where does the test write macroblocks in VRAM?
# Check VRAM shadow rows 0 and 1 starting at col 0
print('\nShadow VRAM row 0, cols 0-15:')
for x in range(16):
    v = int(shadow[0, x])
    r = (v & 0x1F) << 3
    g = ((v >> 5) & 0x1F) << 3
    b = ((v >> 10) & 0x1F) << 3
    print(f'  [{x:2d}] {v:#06x} -> ({r:3d},{g:3d},{b:3d})')

print('\nShadow VRAM row 1, cols 0-15:')
for x in range(16):
    v = int(shadow[1, x])
    r = (v & 0x1F) << 3
    g = ((v >> 5) & 0x1F) << 3
    b = ((v >> 10) & 0x1F) << 3
    print(f'  [{x:2d}] {v:#06x} -> ({r:3d},{g:3d},{b:3d})')

# Compare: check if DMA1 row 0 matches VRAM row 0 cols 0-15
dma_row0 = dma[0:16]
vram_row0 = shadow[0, 0:16]
if np.array_equal(dma_row0, vram_row0):
    print('\n*** DMA1 MB0 row0 == VRAM row0 cols 0-15 ***')
else:
    print('\n*** DMA1 MB0 row0 != VRAM row0 cols 0-15 ***')
    for i in range(16):
        if dma_row0[i] != vram_row0[i]:
            print(
                f'  [{i}] dma={int(dma_row0[i]):#06x} vram={int(vram_row0[i]):#06x}')

# Check if macroblocks are arranged in VRAM as 16x16 tiles in a 320-wide strip
# First MB should be at VRAM (0,0), second at (16,0), etc.
# Or they could be stored linearly (all of MB0 then all of MB1)
print('\nChecking macroblock tiling in VRAM:')
mb0_row1_dma = dma[16:32]
vram_row0_cols16_31 = shadow[0, 16:32]
vram_row1_cols0_15 = shadow[1, 0:16]

if np.array_equal(mb0_row1_dma, vram_row0_cols16_31):
    print('  MB0 row1 == VRAM row0 cols 16-31 (linear layout)')
elif np.array_equal(mb0_row1_dma, vram_row1_cols0_15):
    print('  MB0 row1 == VRAM row1 cols 0-15 (16x16 tile layout)')
else:
    print('  MB0 row1 != VRAM row0 cols16-31 AND != VRAM row1 cols0-15')
    print(
        f'  DMA: [{int(mb0_row1_dma[0]):#06x}, {int(mb0_row1_dma[1]):#06x}, ...]')
    print(
        f'  VR0c16: [{int(vram_row0_cols16_31[0]):#06x}, {int(vram_row0_cols16_31[1]):#06x}, ...]')
    print(
        f'  VR1c0: [{int(vram_row1_cols0_15[0]):#06x}, {int(vram_row1_cols0_15[1]):#06x}, ...]')

# Check where MB1 starts in VRAM (offset 256 in DMA = MB1 row0)
mb1_row0 = dma[256:272]
for y in range(20):
    for x_start in [0, 16, 32, 48]:
        if np.array_equal(mb1_row0, shadow[y, x_start:x_start+16]):
            print(f'  MB1 row0 found at VRAM ({x_start},{y})')
            break
