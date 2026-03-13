#!/usr/bin/env python3
"""Verify MDEC IDCT output for q_scale=0 test blocks."""
import numpy as np

# Standard MDEC scale table (from psx-spx)
scale_table = np.array([
    23170,  23170,  23170,  23170,  23170,  23170,  23170,  23170,
    32138,  27245,  18204,   6392,  -6393, -18205, -27246, -32139,
    30273,  12539, -12540, -30274, -30274, -12540,  12539,  30273,
    27245,  -6393, -32139, -18205,  18204,  32138,   6392, -27246,
    23170, -23171, -23171,  23170,  23170, -23171, -23171,  23170,
    18204, -32139,   6392,  27245, -27246,  -6393,  32138, -18205,
    12539, -30274,  30273, -12540, -12540,  30273, -30274,  12539,
    6392, -18205,  27245, -32139,  32138, -27246,  18204,  -6393
], dtype=np.int64).reshape(8, 8)


def sign_extend_10(val):
    val = val & 0x3FF
    if val & 0x200:
        val -= 0x400
    return val


def duckstation_idct(blk_flat):
    """DuckStation-style real IDCT using scale table.
    Uses flat array access to exactly match DuckStation's blk[y + z*8]."""
    scale_flat = scale_table.flatten()
    temp_flat = np.zeros(64, dtype=np.int64)
    # Pass 1: temp[x + y*8] = sum over z of blk[y + z*8] * scale[x + z*8]
    for x in range(8):
        for y in range(8):
            s = np.int64(0)
            for z in range(8):
                s += int(blk_flat[y + z * 8]) * int(scale_flat[x + z * 8])
            temp_flat[x + y * 8] = (s + 0x4000) >> 15
    # Pass 2: out[x + y*8] = sum over z of temp[y + z*8] * scale[x + z*8]
    out_flat = np.zeros(64, dtype=np.int64)
    for x in range(8):
        for y in range(8):
            s = np.int64(0)
            for z in range(8):
                s += int(temp_flat[y + z * 8]) * int(scale_flat[x + z * 8])
            out_flat[x + y * 8] = max(-32768, min(32767, (s + 0x4000) >> 15))
    return out_flat.reshape(8, 8)


def y_to_mono_unsigned(blk_2d):
    """PSX y_to_mono: AND 0x1FF, clamp -128..127, XOR 0x80."""
    result = np.zeros_like(blk_2d)
    for r in range(8):
        for c in range(8):
            y = int(blk_2d[r, c])
            y = y & 0x1FF
            if y >= 256:
                y -= 512
            y = max(-128, min(127, y))
            y = y ^ 0x80  # +128 unsigned
            if y < 0:
                y += 256
            result[r, c] = y
    return result


# === DCTA: DC=863, q_scale=0 ===
print("=== DCTA: DC=863, q_scale=0 ===")
dc_a = sign_extend_10(863)
print(f"  DC raw sign-extended: {dc_a}")
blk_a = np.zeros(64, dtype=np.int64)
blk_a[0] = dc_a * 2
out_a = duckstation_idct(blk_a)
mono_a = y_to_mono_unsigned(out_a)
idx_a = mono_a >> 4
print(f"  IDCT out[0,0] = {out_a[0, 0]}")
print(f"  Mono[0,0] = {mono_a[0, 0]}")
print(f"  4-bit index row 0: {list(idx_a[0])}")

# === DCTB: DC=864, AC[1]=50, q_scale=0 ===
print("\n=== DCTB: DC=864, AC[1]=50, q_scale=0 ===")
dc_b = sign_extend_10(864)
ac_b = sign_extend_10(50)
print(f"  DC raw: {dc_b}, AC1 raw: {ac_b}")
blk_b = np.zeros(64, dtype=np.int64)
blk_b[0] = dc_b * 2  # -320
blk_b[1] = ac_b * 2  # 100
out_b = duckstation_idct(blk_b)
mono_b = y_to_mono_unsigned(out_b)
idx_b = mono_b >> 4
print(f"  IDCT out row 0: {list(out_b[0])}")
print(f"  Mono row 0: {list(mono_b[0])}")
print(f"  4-bit indices row 0: {list(idx_b[0])}")
print(f"  4-bit indices all rows:")
for r in range(8):
    print(f"    row {r}: {list(idx_b[r])}")

# === What if DC is NOT sign-extended (treated as unsigned)? ===
print("\n=== DCTB: DC=864 UNSIGNED, AC[1]=50 ===")
blk_c = np.zeros(64, dtype=np.int64)
blk_c[0] = 864 * 2  # 1728 unsigned
blk_c[1] = 50 * 2   # 100
out_c = duckstation_idct(blk_c)
mono_c = y_to_mono_unsigned(out_c)
idx_c = mono_c >> 4
print(f"  IDCT out row 0: {list(out_c[0])}")
print(f"  Mono row 0: {list(mono_c[0])}")
print(f"  4-bit indices row 0: {list(idx_c[0])}")

# === What if we DON'T multiply by 2? ===
print("\n=== DCTB: DC=864 sign-ext, NO x2 ===")
blk_d = np.zeros(64, dtype=np.int64)
blk_d[0] = dc_b  # -160, NOT multiplied by 2
blk_d[1] = ac_b  # 50, NOT multiplied by 2
out_d = duckstation_idct(blk_d)
mono_d = y_to_mono_unsigned(out_d)
idx_d = mono_d >> 4
print(f"  IDCT out row 0: {list(out_d[0])}")
print(f"  Mono row 0: {list(mono_d[0])}")
print(f"  4-bit indices row 0: {list(idx_d[0])}")
