#!/usr/bin/env python3
"""Offline MDEC decoder: compare DuckStation OLD (linear q_scale=0) vs NEW (zigzag) vs reference PNG."""
import struct
import sys
import math
import numpy as np
from PIL import Image

# --- Constants ---
MDEC_END = 0xFE00

# Standard PSX quant tables (from ps1-tests/common/mdec.cpp)
quant_y = [
    0x02, 0x10, 0x10, 0x13, 0x10, 0x13, 0x16, 0x16,
    0x16, 0x16, 0x16, 0x16, 0x1a, 0x18, 0x1a, 0x1b,
    0x1b, 0x1b, 0x1a, 0x1a, 0x1a, 0x1a, 0x1b, 0x1b,
    0x1b, 0x1d, 0x1d, 0x1d, 0x22, 0x22, 0x22, 0x1d,
    0x1d, 0x1d, 0x1b, 0x1b, 0x1d, 0x1d, 0x20, 0x20,
    0x22, 0x22, 0x25, 0x26, 0x25, 0x23, 0x23, 0x22,
    0x23, 0x26, 0x26, 0x28, 0x28, 0x28, 0x30, 0x30,
    0x2e, 0x2e, 0x38, 0x38, 0x3a, 0x45, 0x45, 0x53,
]
quant_uv = quant_y[:]  # same in ps1-tests

# Standard IDCT scale table (from ps1-tests/common/mdec.cpp)
idct_table = [
    23170, 23170, 23170, 23170, 23170, 23170, 23170, 23170,
    32138, 27245, 18204,  6392, -6393, -18205, -27246, -32139,
    30273, 12539, -12540, -30274, -30274, -12540, 12539, 30273,
    27245, -6393, -32139, -18205, 18204, 32138,  6392, -27246,
    23170, -23171, -23171, 23170, 23170, -23171, -23171, 23170,
    18204, -32139,  6392, 27245, -27246, -6393, 32138, -18205,
    12539, -30274, 30273, -12540, -12540, 30273, -30274, 12539,
    6392, -18205, 27245, -32139, 32138, -27246, 18204, -6393,
]

# Zigzag table (column-major, for DuckStation OLD IDCT)
zagzig_old = [
    0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
]

# Zigzag table (row-major, for DuckStation NEW IDCT)
zigzag_new = [
    0,  8,  1,  2,  9, 16, 24, 17,
    10,  3,  4, 11, 18, 25, 32, 40,
    33, 26, 19, 12,  5,  6, 13, 20,
    27, 34, 41, 48, 56, 49, 42, 35,
    28, 21, 14,  7, 15, 22, 29, 36,
    43, 50, 57, 58, 51, 44, 37, 30,
    23, 31, 38, 45, 52, 59, 60, 53,
    46, 39, 47, 54, 61, 62, 55, 63,
]


def sign10(val):
    """Sign-extend 10-bit value."""
    if val & 0x200:
        return val - 0x400
    return val


def clamp(val, lo, hi):
    return max(lo, min(hi, val))


def sign_extend_9(val):
    """Sign-extend 9 bits."""
    val = val & 0x1FF
    if val & 0x100:
        return val - 0x200
    return val

# --- DuckStation OLD approach ---


def decode_rle_old(halfwords, offset, qt):
    """Decode one 8x8 block using DuckStation OLD method. Returns (blk[64], new_offset)."""
    blk = [0] * 64
    start_offset = offset

    n = halfwords[offset]
    offset += 1
    q_scale = (n >> 10) & 0x3F
    val = sign10(n & 0x3FF)

    # DC
    if q_scale == 0:
        dc = val * 2
    else:
        dc = val * qt[0]
    dc = clamp(dc, -0x400, 0x3FF)

    if q_scale > 0:
        blk[zagzig_old[0]] = dc
    else:
        blk[0] = dc  # linear

    k = 0
    coeffs_detail = [(0, val, dc)]  # (k, raw_val, stored_val)
    while True:
        n = halfwords[offset]
        offset += 1
        if n == MDEC_END:
            break
        k += ((n >> 10) & 0x3F) + 1
        if k > 63:
            break

        val = sign10(n & 0x3FF)
        if q_scale == 0:
            ac = val * 2
        else:
            ac = (val * qt[k] * q_scale + 4) // 8
        ac = clamp(ac, -0x400, 0x3FF)

        coeffs_detail.append((k, val, ac))

        if q_scale > 0:
            blk[zagzig_old[k]] = ac
        else:
            blk[k] = ac  # linear

    return blk, offset, q_scale, start_offset, coeffs_detail


def idct_old(blk, scale_table):
    """DuckStation OLD IDCT (column-major)."""
    # scale_table is [y*8+u] for 8x8
    temp = [0] * 64
    for x in range(8):
        for y in range(8):
            s = 0
            for u in range(8):
                s += blk[u * 8 + x] * scale_table[y * 8 + u]
            temp[x + y * 8] = s

    out = [0] * 64
    for x in range(8):
        for y in range(8):
            s = 0
            for u in range(8):
                s += temp[u + y * 8] * scale_table[x * 8 + u]
            # sign extend 9 bits after rounding
            result = (s >> 32) + ((s >> 31) & 1)
            result = sign_extend_9(result)
            out[x + y * 8] = clamp(result, -128, 127)
    return out

# --- DuckStation NEW approach ---


def decode_rle_new(halfwords, offset, qt):
    """Decode one 8x8 block using DuckStation NEW method. Returns (blk[64], new_offset)."""
    blk = [0] * 64
    start_offset = offset

    n = halfwords[offset]
    offset += 1
    q_scale = (n >> 10) & 0x3F
    val = sign10(n & 0x3FF)

    # DC with 4 extra bits of precision
    if q_scale == 0:
        coeff = val << 5
    else:
        coeff = (val * qt[0]) << 4
        if val != 0:
            coeff += 8 if val < 0 else -8
    coeff = clamp(coeff, -0x4000, 0x3FFF)
    blk[zigzag_new[0]] = coeff

    k = 0
    while True:
        n = halfwords[offset]
        offset += 1
        if n == MDEC_END:
            break
        k += ((n >> 10) & 0x3F) + 1
        if k > 63:
            break

        val = sign10(n & 0x3FF)
        scq = q_scale * qt[k]
        if scq == 0:
            coeff = val << 5
        else:
            coeff = ((val * scq) >> 3) << 4
            if val != 0:
                coeff += 8 if val < 0 else -8
        coeff = clamp(coeff, -0x4000, 0x3FFF)
        blk[zigzag_new[k]] = coeff

    return blk, offset, q_scale, start_offset


def idct_row(blk_row, scale_row):
    """Compute one IDCT output sample (DuckStation NEW style with MADD)."""
    s = sum(blk_row[i] * scale_row[i] for i in range(8))
    return (s + 0x20000) >> 18


def idct_new(blk, scale_table):
    """DuckStation NEW IDCT (row-major)."""
    temp = [0] * 64
    for x in range(8):
        for y in range(8):
            temp[y * 8 + x] = idct_row(blk[x*8:x*8+8], scale_table[y*8:y*8+8])

    out = [0] * 64
    for x in range(8):
        for y in range(8):
            s = idct_row(temp[x*8:x*8+8], scale_table[y*8:y*8+8])
            s = sign_extend_9(s)
            out[x * 8 + y] = clamp(s, -128, 127)
    return out

# --- YUV to RGB (15-bit unsigned, NOT signed output) ---


def yuv_to_rgb15_old(Cr_blk, Cb_blk, Y_blk, xx, yy):
    """Convert one 8x8 Y block to 15-bit RGB pixels. Returns 8x8 array of (r5,g5,b5)."""
    pixels = []
    for y in range(8):
        row = []
        for x in range(8):
            R_coeff = Cr_blk[((x + xx) // 2) + ((y + yy) // 2) * 8]
            B_coeff = Cb_blk[((x + xx) // 2) + ((y + yy) // 2) * 8]
            G_adj = int(-0.3437 * B_coeff + -0.7143 * R_coeff)
            R_adj = int(1.402 * R_coeff)
            B_adj = int(1.772 * B_coeff)
            Y_val = Y_blk[x + y * 8]

            r = clamp(Y_val + R_adj, -128, 127) + 128
            g = clamp(Y_val + G_adj, -128, 127) + 128
            b = clamp(Y_val + B_adj, -128, 127) + 128

            # 8-bit to 5-bit (truncate)
            row.append((r >> 3, g >> 3, b >> 3))
        pixels.append(row)
    return pixels


def yuv_to_rgb15_new(Cr_blk, Cb_blk, Y_blk, xx, yy):
    """Convert using DuckStation NEW method (Mednafen-style)."""
    pixels = []
    for y in range(8):
        row = []
        for x in range(8):
            Cr = Cr_blk[((x + xx) // 2) + ((y + yy) // 2) * 8]
            Cb = Cb_blk[((x + xx) // 2) + ((y + yy) // 2) * 8]
            Y_val = Y_blk[x + y * 8]

            r_adj = (359 * Cr + 0x80) >> 8
            b_adj = (454 * Cb + 0x80) >> 8
            g_adj = (((-88 * Cb) & ~0x1F) + ((-183 * Cr) & ~0x07) + 0x80) >> 8

            r = sign_extend_9(Y_val + r_adj)
            g = sign_extend_9(Y_val + g_adj)
            b = sign_extend_9(Y_val + b_adj)
            r = clamp(r, -128, 127) + 128
            g = clamp(g, -128, 127) + 128
            b = clamp(b, -128, 127) + 128

            # 8-bit to 5-bit with rounding: (c + 4) >> 3, clamp to 31
            r5 = min((r + 4) >> 3, 31)
            g5 = min((g + 4) >> 3, 31)
            b5 = min((b + 4) >> 3, 31)
            row.append((r5, g5, b5))
        pixels.append(row)
    return pixels


def decode_macroblock_old(halfwords, offset, qt_uv, qt_y, debug=False):
    """Decode one macroblock (Cr,Cb,Y1,Y2,Y3,Y4) using OLD method."""
    blocks_raw = []  # before IDCT
    blocks = []
    qscales = []
    block_details = []
    for i in range(6):
        qt = qt_uv if i < 2 else qt_y
        blk, offset, qs, start_off, coeffs = decode_rle_old(
            halfwords, offset, qt)
        blocks_raw.append(blk[:])
        block_details.append((start_off, coeffs))
        blk = idct_old(blk, idct_table)
        blocks.append(blk)
        qscales.append(qs)

    if debug:
        names = ["Cr", "Cb", "Y1", "Y2", "Y3", "Y4"]
        for i in range(6):
            raw = blocks_raw[i]
            post = blocks[i]
            nz = sum(1 for v in raw if v != 0)
            start_off, coeffs = block_details[i]
            print(
                f"  {names[i]}: q={qscales[i]}, offset={start_off}, nonzero_coeffs={nz}, DC_raw={raw[0]}")
            print(f"    header: 0x{halfwords[start_off]:04x}")
            print(
                f"    coefficients (k, raw, stored): {coeffs[:5]}{'...' if len(coeffs) > 5 else ''}")
            print(f"    pre-IDCT [0:8]: {raw[0:8]}")
            print(f"    post-IDCT [0:8]: {post[0:8]}")
            print(
                f"    post-IDCT mean={sum(post)/64:.1f}, min={min(post)}, max={max(post)}")

    Cr, Cb, Y1, Y2, Y3, Y4 = blocks

    # 16x16 pixel output
    pixels = [[None]*16 for _ in range(16)]
    for yblk, xx, yy in [(Y1, 0, 0), (Y2, 8, 0), (Y3, 0, 8), (Y4, 8, 8)]:
        p = yuv_to_rgb15_old(Cr, Cb, yblk, xx, yy)
        for dy in range(8):
            for dx in range(8):
                pixels[yy + dy][xx + dx] = p[dy][dx]

    return pixels, offset, qscales


def decode_macroblock_new(halfwords, offset, qt_uv, qt_y):
    """Decode one macroblock (Cr,Cb,Y1,Y2,Y3,Y4) using NEW method."""
    blocks = []
    qscales = []
    for i in range(6):
        qt = qt_uv if i < 2 else qt_y
        blk, offset, qs, _ = decode_rle_new(halfwords, offset, qt)
        blk = idct_new(blk, idct_table)
        blocks.append(blk)
        qscales.append(qs)

    Cr, Cb, Y1, Y2, Y3, Y4 = blocks

    pixels = [[None]*16 for _ in range(16)]
    for yblk, xx, yy in [(Y1, 0, 0), (Y2, 8, 0), (Y3, 0, 8), (Y4, 8, 8)]:
        p = yuv_to_rgb15_new(Cr, Cb, yblk, xx, yy)
        for dy in range(8):
            for dx in range(8):
                pixels[yy + dy][xx + dx] = p[dy][dx]

    return pixels, offset, qscales


def load_reference_mb0(ref_path):
    """Load reference PNG and extract first macroblock (16x16 pixels at top-left)."""
    img = Image.open(ref_path)
    # Reference is 1024x512 VRAM dump. First MB at pixel (0,0).
    pixels = []
    for y in range(16):
        row = []
        for x in range(16):
            r, g, b = img.getpixel((x, y))[:3]
            row.append((r // 8, g // 8, b // 8))
        pixels.append(row)
    return pixels


def compare_mb(name, decoded, reference):
    """Compare decoded vs reference macroblock."""
    exact = 0
    close = 0
    far = 0
    total_diff = 0
    max_diff = 0

    for y in range(16):
        for x in range(16):
            d = decoded[y][x]
            r = reference[y][x]
            diff = max(abs(d[0]-r[0]), abs(d[1]-r[1]), abs(d[2]-r[2]))
            total_diff += diff
            max_diff = max(max_diff, diff)
            if diff == 0:
                exact += 1
            elif diff <= 1:
                close += 1
            else:
                far += 1

    total = 16 * 16
    mean = total_diff / total
    print(f"\n=== {name} ===")
    print(
        f"Exact: {exact}/{total}, Close(±1): {close}/{total}, Far(>1): {far}/{total}")
    print(f"Mean diff: {mean:.2f}, Max diff: {max_diff}")

    # Show first row
    print(f"Decoded  row0: {[decoded[0][x] for x in range(8)]}")
    print(f"Reference row0: {[reference[0][x] for x in range(8)]}")

    # Show Y3 area (row 8, cols 0-7)
    print(f"Decoded  Y3[0]: {[decoded[8][x] for x in range(8)]}")
    print(f"Reference Y3[0]: {[reference[8][x] for x in range(8)]}")

    return exact, close, far, mean


def main():
    mdec_path = "/Users/frangar/Fun/ps2/ps1-tests/mdec/frame/sunset.mdec"
    ref_path = "/Users/frangar/Fun/ps2/superpsx/tests/mdec/frame/vram-15bit.png"

    if not __import__('os').path.exists(ref_path):
        ref_path = "/Users/frangar/Fun/ps2/ps1-tests/mdec/frame/vram-15bit.png"

    # Load MDEC data as little-endian uint16 array
    with open(mdec_path, 'rb') as f:
        data = f.read()
    halfwords = struct.unpack(f'<{len(data)//2}H', data)
    print(f"Loaded {len(halfwords)} halfwords from sunset.mdec")

    # Load reference
    ref_mb0 = load_reference_mb0(ref_path)

    # Print first few halfwords
    print(f"First 10 halfwords: {[f'0x{h:04x}' for h in halfwords[:10]]}")

    # Show q_scale of first few blocks
    print(f"\nMB0 block headers:")
    offset = 0
    for i, name in enumerate(["Cr", "Cb", "Y1", "Y2", "Y3", "Y4"]):
        h = halfwords[offset]
        q = (h >> 10) & 0x3F
        dc = sign10(h & 0x3FF)
        print(f"  {name}: offset={offset}, raw=0x{h:04x}, q_scale={q}, dc_raw={dc}")
        # Skip to next block
        offset += 1
        while offset < len(halfwords):
            if halfwords[offset] == MDEC_END:
                offset += 1
                break
            offset += 1

    # Dump all halfwords consumed by MB0
    print("\n--- MB0 halfword dump (all 69) ---")
    for i in range(min(70, len(halfwords))):
        hw = halfwords[i]
        qs = (hw >> 10) & 0x3F
        val = sign10(hw & 0x3FF)
        marker = " **FE00**" if hw == MDEC_END else ""
        print(f"  [{i:3d}] 0x{hw:04x}  run={qs:2d} val={val:5d}{marker}")

    # Decode MB0 with both methods
    print("\n--- Decoding MB0 ---")

    pixels_old, off_old, qs_old = decode_macroblock_old(
        halfwords, 0, quant_uv, quant_y, debug=True)
    print(f"OLD: q_scales={qs_old}, consumed {off_old} halfwords")

    pixels_new, off_new, qs_new = decode_macroblock_new(
        halfwords, 0, quant_uv, quant_y)
    print(f"NEW: q_scales={qs_new}, consumed {off_new} halfwords")

    # Compare
    compare_mb("DuckStation OLD (linear q=0)", pixels_old, ref_mb0)
    compare_mb("DuckStation NEW (zigzag q=0)", pixels_new, ref_mb0)

    # Try treating q_scale=0 as q_scale=1 (no special handling)
    print("\n\n=== q_scale=0 treated as q_scale=1 ===")
    offset_q1 = 0
    blocks_q1 = []
    for i in range(6):
        qt = quant_uv if i < 2 else quant_y
        blk = [0] * 64
        n = halfwords[offset_q1]
        offset_q1 += 1
        q_scale = (n >> 10) & 0x3F
        val = sign10(n & 0x3FF)
        effective_q = q_scale if q_scale > 0 else 1  # treat 0 as 1
        blk[zagzig_old[0]] = clamp(val * qt[0], -0x400, 0x3FF)
        k = 0
        while True:
            n = halfwords[offset_q1]
            offset_q1 += 1
            if n == MDEC_END:
                break
            k += ((n >> 10) & 0x3F) + 1
            if k > 63:
                break
            val = sign10(n & 0x3FF)
            ac = (val * qt[k] * effective_q + 4) // 8
            ac = clamp(ac, -0x400, 0x3FF)
            blk[zagzig_old[k]] = ac  # always zigzag
        if k == 0:
            used_col = -1
        blk = idct_old(blk, idct_table)
        blocks_q1.append(blk)
    Cr, Cb, Y1, Y2, Y3, Y4 = blocks_q1
    pixels_q1 = [[None]*16 for _ in range(16)]
    for yblk, xx, yy in [(Y1, 0, 0), (Y2, 8, 0), (Y3, 0, 8), (Y4, 8, 8)]:
        p = yuv_to_rgb15_old(Cr, Cb, yblk, xx, yy)
        for dy in range(8):
            for dx in range(8):
                pixels_q1[yy + dy][xx + dx] = p[dy][dx]
    compare_mb("q_scale=0 treated as q_scale=1", pixels_q1, ref_mb0)

    # DuckStation NEW-style loop termination: exit at k >= 63
    print("\n\n=== DuckStation NEW-style k>=63 exit (proper FE00 skip) ===")
    offset_ds = 0
    blocks_ds = []
    qscales_ds = []
    for i in range(6):
        qt = quant_uv if i < 2 else quant_y
        blk = [0] * 64
        start_ds = offset_ds

        # Skip FE00 padding at block start
        while halfwords[offset_ds] == MDEC_END:
            offset_ds += 1

        n = halfwords[offset_ds]
        offset_ds += 1
        q_scale = (n >> 10) & 0x3F
        val = sign10(n & 0x3FF)
        dc = val * qt[0] if q_scale > 0 else val * 2
        dc = clamp(dc, -0x400, 0x3FF)
        blk[zagzig_old[0]] = dc  # always zigzag for NEW

        k = 0
        while True:
            n = halfwords[offset_ds]
            offset_ds += 1
            k += ((n >> 10) & 0x3F) + 1
            if k < 64:
                val = sign10(n & 0x3FF)
                if q_scale == 0:
                    ac = val * 2
                else:
                    ac = (val * qt[k] * q_scale + 4) // 8
                ac = clamp(ac, -0x400, 0x3FF)
                blk[zagzig_old[k]] = ac
            if k >= 63:
                break  # exit immediately, like DuckStation NEW

        names_ds = ["Cr", "Cb", "Y1", "Y2", "Y3", "Y4"]
        post = idct_old(blk, idct_table)
        print(
            f"  {names_ds[i]}: q={q_scale}, start={start_ds}, header=0x{halfwords[start_ds]:04x}, DC={dc}, IDCT_mean={sum(post)/64:.1f}")
        blocks_ds.append(post)
        qscales_ds.append(q_scale)
    Cr, Cb, Y1, Y2, Y3, Y4 = blocks_ds
    pixels_ds = [[None]*16 for _ in range(16)]
    for yblk, xx, yy in [(Y1, 0, 0), (Y2, 8, 0), (Y3, 0, 8), (Y4, 8, 8)]:
        p = yuv_to_rgb15_old(Cr, Cb, yblk, xx, yy)
        for dy in range(8):
            for dx in range(8):
                pixels_ds[yy + dy][xx + dx] = p[dy][dx]
    compare_mb("DuckStation NEW-style k>=63", pixels_ds, ref_mb0)

    # Try DC accumulation (MPEG-style differential DC)
    print("\n\n=== Testing DC accumulation approach ===")
    dc_accum = 0
    offset_acc = 0
    blocks_acc = []
    qscales_acc = []
    for i in range(6):
        qt = quant_uv if i < 2 else quant_y
        blk, offset_acc, qs, start_off, coeffs = decode_rle_old(
            halfwords, offset_acc, qt)
        # Apply DC accumulation: DC += previous DC
        dc_val = blk[0]  # already = sign10*qt[0] or sign10*2 (for q=0)
        dc_accum += dc_val
        dc_accum = clamp(dc_accum, -0x400, 0x3FF)
        blk[0] = dc_accum
        names_acc = ["Cr", "Cb", "Y1", "Y2", "Y3", "Y4"]
        if i >= 2:  # Y blocks
            print(f"  {names_acc[i]}: dc_raw={dc_val}, dc_accum={dc_accum}")
        blk = idct_old(blk, idct_table)
        blocks_acc.append(blk)
        qscales_acc.append(qs)

    Cr_a, Cb_a, Y1_a, Y2_a, Y3_a, Y4_a = blocks_acc
    pixels_acc = [[None]*16 for _ in range(16)]
    for yblk, xx, yy in [(Y1_a, 0, 0), (Y2_a, 8, 0), (Y3_a, 0, 8), (Y4_a, 8, 8)]:
        p = yuv_to_rgb15_old(Cr_a, Cb_a, yblk, xx, yy)
        for dy in range(8):
            for dx in range(8):
                pixels_acc[yy + dy][xx + dx] = p[dy][dx]
    compare_mb("DC Accumulation (single accum)", pixels_acc, ref_mb0)

    # Try separate DC accumulators per component type
    print("\n\n=== Testing separate DC accumulation (Cr/Cb/Y) ===")
    dc_cr_acc = 0
    dc_cb_acc = 0
    dc_y_acc = 0
    offset_sep = 0
    blocks_sep = []
    for i in range(6):
        qt = quant_uv if i < 2 else quant_y
        blk, offset_sep, qs, start_off, coeffs = decode_rle_old(
            halfwords, offset_sep, qt)
        dc_val = blk[0]
        if i == 0:
            dc_cr_acc += dc_val
            dc_cr_acc = clamp(dc_cr_acc, -0x400, 0x3FF)
            blk[0] = dc_cr_acc
        elif i == 1:
            dc_cb_acc += dc_val
            dc_cb_acc = clamp(dc_cb_acc, -0x400, 0x3FF)
            blk[0] = dc_cb_acc
        else:
            dc_y_acc += dc_val
            dc_y_acc = clamp(dc_y_acc, -0x400, 0x3FF)
            blk[0] = dc_y_acc
            names_sep = ["", "", "Y1", "Y2", "Y3", "Y4"]
            print(f"  {names_sep[i]}: dc_raw={dc_val}, dc_y_acc={dc_y_acc}")
        blk = idct_old(blk, idct_table)
        blocks_sep.append(blk)

    Cr_s, Cb_s, Y1_s, Y2_s, Y3_s, Y4_s = blocks_sep
    pixels_sep = [[None]*16 for _ in range(16)]
    for yblk, xx, yy in [(Y1_s, 0, 0), (Y2_s, 8, 0), (Y3_s, 0, 8), (Y4_s, 8, 8)]:
        p = yuv_to_rgb15_old(Cr_s, Cb_s, yblk, xx, yy)
        for dy in range(8):
            for dx in range(8):
                pixels_sep[yy + dy][xx + dx] = p[dy][dx]
    compare_mb("DC Sep Accumulation (per-component)", pixels_sep, ref_mb0)

    # Detailed block comparison for Y3 area
    print("\n--- Y3 area detailed (rows 8-15, cols 0-7) ---")
    for y in range(8, 16):
        old_row = [pixels_old[y][x] for x in range(8)]
        new_row = [pixels_new[y][x] for x in range(8)]
        ref_row = [ref_mb0[y][x] for x in range(8)]

        old_diff = [max(abs(old_row[x][c]-ref_row[x][c])
                        for c in range(3)) for x in range(8)]
        new_diff = [max(abs(new_row[x][c]-ref_row[x][c])
                        for c in range(3)) for x in range(8)]

        print(f"  y={y}: OLD_diff={old_diff}  NEW_diff={new_diff}")


if __name__ == "__main__":
    main()
