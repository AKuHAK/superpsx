#!/usr/bin/env python3
import numpy as np
from PIL import Image

vram = np.fromfile("build/vram_dump.bin", dtype=np.uint16).reshape(512, 1024)
ref = np.array(Image.open("tests/mdec/8bit/vram.png").convert("L"))

print("VRAM uint16 at (32,32)-(39,35):")
for y in range(32, 40):
    vals = " ".join(f"{vram[y, x]:04x}" for x in range(32, 36))
    print(f"  row {y}: {vals}")

print()
print("Ref uint8 at (32,32)-(39,39):")
for y in range(32, 40):
    vals = " ".join(f"{ref[y, x]:3d}" for x in range(32, 40))
    print(f"  row {y}: {vals}")

print()
print("VRAM byte pairs (lo,hi) at (32,32)-(35,32):")
for y in range(32, 40):
    pairs = " ".join(
        f"({vram[y, x] & 0xFF:3d},{vram[y, x] >> 8:3d})" for x in range(32, 36))
    print(f"  row {y}: {pairs}")

# The expected 8-bit data from psx.log:
expected = [0x00, 0xff, 0xff, 0x00, 0xff, 0xff, 0x04, 0x00,
            0xc9, 0xec, 0xef, 0xed, 0xef, 0xfc, 0xf2, 0x00,
            0xd5, 0xdb, 0xfa, 0xe8, 0xfe, 0xe8, 0xff, 0x00,
            0xb7, 0xf3, 0xec, 0xef, 0xeb, 0xff, 0xe3, 0x00,
            0x00, 0xfb, 0xff, 0xf5, 0xf2, 0xff, 0x03, 0x00,
            0x00, 0x05, 0xff, 0xfc, 0xff, 0x08, 0x1a, 0x00,
            0x0f, 0x28, 0x1e, 0xff, 0x05, 0x2a, 0x23, 0x00,
            0x10, 0x38, 0x40, 0x29, 0x32, 0x32, 0x16, 0x0f]
print()
print("Expected 8-bit bytes (from psx.log):")
for row in range(8):
    vals = " ".join(f"{expected[row*8+i]:3d}" for i in range(8))
    print(f"  row {row}: {vals}")
