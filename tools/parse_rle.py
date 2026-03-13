#!/usr/bin/env python3
"""Manually decode MDEC RLE stream and verify block boundaries."""
import sys

# Raw RLE data from trace (first 128 halfwords)
rle_hex = """
07F1 0BFF 03FF E800 0411 07FE 03FF 0001 E800 FE00 076D 03FE 000E 03FF
03FF 0001 0403 0001 03FE 0BFF 07FE 0C01 03FF 13FF 9800 FE00 0751 000B 000A 0001
0005 0006 0402 0001 03FF 0001 03FF 03FF 13FF 03FF 03FF AC00 0773 0003 03F6 07FF
03FE 0401 07FE 03FF 07FF 0001 03FF 07FF 0001 1801 9800 FE00 075C 0009 03F5 03FF
03FF 03FF 0FFF 0BFF 5BFF 6C00 041C 03FF 03F1 03FC 03FF 03FF 07FF 0001 03FC 0401
CC00 FE00 07EA 0001 000E 0004 0001 0801 03FF 0003 07FE CC00 078C 03FC 0001 03FE
0002 0004 03FE 03FF 0002 0BFF 03FF 0001 03FF 03FE 0C01 0801 07FF 1BFF 7C00 FE00
0795 0009 03F5 03FB 0003 03FD 0002 03FF 03FE 0C01 0002 03FE 0BFF 0001 07FF A800
0780 0007
""".split()

rle = [int(x, 16) for x in rle_hex]

DSIZE2 = 64
MDEC_END = 0xFE00

# Zigzag table
zscan = [
    0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
]


def signed10(v):
    v = v & 0x3FF
    if v & 0x200:
        v -= 0x400
    return v


def rle_run(hw):
    return (hw >> 10) & 0x3F


def rle_val(hw):
    return signed10(hw)


block_names = ["Cr", "Cb", "Y1", "Y2", "Y3", "Y4"]
pos = 0

for mb in range(2):  # Trace 2 macroblocks
    print("=== Macroblock {} ===".format(mb))
    for bi in range(6):
        if pos >= len(rle):
            print("  OVERFLOW: no more data")
            break
        hw = rle[pos]
        pos += 1
        q_scale = rle_run(hw)
        dc_raw = rle_val(hw)

        k = 0
        ac_entries = []

        while pos < len(rle):
            hw = rle[pos]
            pos += 1
            if hw == MDEC_END:
                break
            run = rle_run(hw)
            val = rle_val(hw)
            k += run + 1
            if k >= DSIZE2:
                # Consumed but overflow — entry lost
                break
            ac_entries.append((k, run, val))

        print("  {} [blk{}]: pos_start={}, q_scale={}, dc_raw={}, ac_count={}, final_k={}".format(
            block_names[bi], bi, pos - 1 -
            len(ac_entries) - (1 if k < DSIZE2 else 2),
            q_scale, dc_raw, len(ac_entries), k))
        if bi == 4:  # Y3 - show detail
            print("    AC entries: {}".format(ac_entries[:10]))
    print()
