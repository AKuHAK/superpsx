#!/usr/bin/env python3
"""Debug DCTG (letter A) block decode through our AAN IDCT."""
import numpy as np

# AAN constants matching src/mdec.c
AAN_CONST_BITS = 12
AAN_PRESCALE_BITS = 16
AAN_CONST_SIZE = 24
AAN_CONST_SCALE = AAN_CONST_SIZE - AAN_CONST_BITS  # 12
AAN_PRESCALE_SIZE = 20
AAN_PRESCALE_SCALE = AAN_PRESCALE_SIZE - AAN_PRESCALE_BITS  # 4
AAN_EXTRA = 12

max_intermediate = [0]


def SCALER(x, n):
    max_intermediate[0] = max(max_intermediate[0], abs(x))
    return (x + (1 << (n - 1))) >> n if n > 0 else x


def SCALE(x, n):
    max_intermediate[0] = max(max_intermediate[0], abs(x))
    return x >> n


def MULS(var, c):
    product = var * c
    max_intermediate[0] = max(max_intermediate[0], abs(product))
    return SCALE(product, AAN_CONST_BITS)


FIX_1_082392200 = SCALER(18159528, AAN_CONST_SCALE)
FIX_1_414213562 = SCALER(23726566, AAN_CONST_SCALE)
FIX_1_847759065 = SCALER(31000253, AAN_CONST_SCALE)
FIX_2_613125930 = SCALER(43840978, AAN_CONST_SCALE)

aanscales = [
    1048576, 1454417, 1370031, 1232995, 1048576,  823861, 567485, 289301,
    1454417, 2017334, 1900287, 1710213, 1454417, 1142728, 787125, 401273,
    1370031, 1900287, 1790031, 1610986, 1370031, 1076426, 741455, 377991,
    1232995, 1710213, 1610986, 1449849, 1232995,  968758, 667292, 340183,
    1048576, 1454417, 1370031, 1232995, 1048576,  823861, 567485, 289301,
    823861, 1142728, 1076426,  968758,  823861,  647303, 445870, 227303,
    567485,  787125,  741455,  667292,  567485,  445870, 307121, 156569,
    289301,  401273,  377991,  340183,  289301,  227303, 156569,  79818
]
DSIZE = 8


def aan_only_init():
    return [SCALER(aanscales[i], AAN_PRESCALE_SCALE) for i in range(64)]


def sign_extend_10(val):
    val = val & 0x3FF
    if val & 0x200:
        val -= 0x400
    return val


def idct(blk, used_col):
    """Exact copy of our AAN IDCT from mdec.c."""
    # All-DC shortcut
    if used_col == -1:
        v = blk[0]
        for i in range(64):
            blk[i] = v
        return

    # Column pass
    for i in range(DSIZE):
        if (used_col & (1 << i)) == 0:
            if blk[DSIZE * 0 + i]:
                v = blk[i]
                for j in range(DSIZE):
                    blk[DSIZE * j + i] = v
                used_col |= (1 << i)
            continue

        z10 = blk[DSIZE*0+i] + blk[DSIZE*4+i]
        z11 = blk[DSIZE*0+i] - blk[DSIZE*4+i]
        z13 = blk[DSIZE*2+i] + blk[DSIZE*6+i]
        z12 = MULS(blk[DSIZE*2+i] - blk[DSIZE*6+i], FIX_1_414213562) - z13

        tmp0 = z10 + z13
        tmp3 = z10 - z13
        tmp1 = z11 + z12
        tmp2 = z11 - z12

        z13 = blk[DSIZE*3+i] + blk[DSIZE*5+i]
        z10 = blk[DSIZE*3+i] - blk[DSIZE*5+i]
        z11 = blk[DSIZE*1+i] + blk[DSIZE*7+i]
        z12 = blk[DSIZE*1+i] - blk[DSIZE*7+i]

        tmp7 = z11 + z13
        z5_val = (z12 - z10) * FIX_1_847759065
        max_intermediate[0] = max(max_intermediate[0], abs(z5_val))
        tmp6_val = z10 * FIX_2_613125930 + z5_val
        max_intermediate[0] = max(max_intermediate[0], abs(tmp6_val))
        tmp6 = SCALE(tmp6_val, AAN_CONST_BITS) - tmp7
        tmp5 = MULS(z11 - z13, FIX_1_414213562) - tmp6
        tmp4_val = z12 * FIX_1_082392200 - z5_val
        max_intermediate[0] = max(max_intermediate[0], abs(tmp4_val))
        tmp4 = SCALE(tmp4_val, AAN_CONST_BITS) + tmp5

        blk[DSIZE*0+i] = tmp0 + tmp7
        blk[DSIZE*7+i] = tmp0 - tmp7
        blk[DSIZE*1+i] = tmp1 + tmp6
        blk[DSIZE*6+i] = tmp1 - tmp6
        blk[DSIZE*2+i] = tmp2 + tmp5
        blk[DSIZE*5+i] = tmp2 - tmp5
        blk[DSIZE*4+i] = tmp3 + tmp4
        blk[DSIZE*3+i] = tmp3 - tmp4

    # Row pass (used_col may have been updated)
    if used_col == 1:
        for r in range(DSIZE):
            v = blk[r * DSIZE]
            for c in range(1, DSIZE):
                blk[r * DSIZE + c] = v
    else:
        for r in range(DSIZE):
            ptr = r * DSIZE
            z10 = blk[ptr+0] + blk[ptr+4]
            z11 = blk[ptr+0] - blk[ptr+4]
            z13 = blk[ptr+2] + blk[ptr+6]
            z12 = MULS(blk[ptr+2] - blk[ptr+6], FIX_1_414213562) - z13

            tmp0 = z10 + z13
            tmp3 = z10 - z13
            tmp1 = z11 + z12
            tmp2 = z11 - z12

            z13 = blk[ptr+3] + blk[ptr+5]
            z10 = blk[ptr+3] - blk[ptr+5]
            z11 = blk[ptr+1] + blk[ptr+7]
            z12 = blk[ptr+1] - blk[ptr+7]

            tmp7 = z11 + z13
            z5_val = (z12 - z10) * FIX_1_847759065
            max_intermediate[0] = max(max_intermediate[0], abs(z5_val))
            tmp6_val = z10 * FIX_2_613125930 + z5_val
            max_intermediate[0] = max(max_intermediate[0], abs(tmp6_val))
            tmp6 = SCALE(tmp6_val, AAN_CONST_BITS) - tmp7
            tmp5 = MULS(z11 - z13, FIX_1_414213562) - tmp6
            tmp4_val = z12 * FIX_1_082392200 - z5_val
            max_intermediate[0] = max(max_intermediate[0], abs(tmp4_val))
            tmp4 = SCALE(tmp4_val, AAN_CONST_BITS) + tmp5

            blk[ptr+0] = tmp0 + tmp7
            blk[ptr+7] = tmp0 - tmp7
            blk[ptr+1] = tmp1 + tmp6
            blk[ptr+6] = tmp1 - tmp6
            blk[ptr+2] = tmp2 + tmp5
            blk[ptr+5] = tmp2 - tmp5
            blk[ptr+4] = tmp3 + tmp4
            blk[ptr+3] = tmp3 - tmp4


# DCTG RLE data
dctg_rle = [
    (0, 810), (1, -130), (1, -100), (1, -54), (1, -53), (1, -98),
    (1, 128), (1, 98), (1, -92), (1, 50), (1, 92), (1, -120),
    (1, 108), (1, -83), (1, 45), (1, -83), (9, -72), (1, 55),
    (1, -30), (1, 55), (1, 38), (1, -20), (1, -38), (1, 50),
    (1, 10), (1, 19), (1, -25), (1, -19), (0, 0)  # last entry before FE00
]

aan_only = aan_only_init()
blk = [0] * 64
used_col = 0

# Decode header (first entry)
run0, val0 = dctg_rle[0]
dc_raw = sign_extend_10(val0)
blk[0] = SCALER(dc_raw * 2 * aan_only[0], AAN_EXTRA - 3)
k = 0

# Decode AC entries
for i in range(1, len(dctg_rle)):
    run, val = dctg_rle[i]
    val = sign_extend_10(val)
    k += run + 1
    if k < 64:
        blk[k] = SCALER(val * 2 * aan_only[k], AAN_EXTRA - 3)
        if k > 7:
            used_col |= 1 << (k & 7)

print(f"DC raw = {dc_raw}, DC scaled = {blk[0]}")
print(f"k after loop = {k}")
print(f"used_col = 0x{used_col:02x} = {bin(used_col)}")
print()

# Show block before IDCT
print("Block before IDCT (non-zero entries):")
for i in range(64):
    if blk[i] != 0:
        r, c = i // 8, i % 8
        print(f"  blk[{i}] = {blk[i]:10d}  (row={r}, col={c})")
print()

# Run IDCT
idct(blk, used_col)

# Convert to mono 4-bit
print(f"\nMax intermediate value: {max_intermediate[0]}")
print(f"Fits in 32-bit signed: {max_intermediate[0] < 2**31}")
for r in range(8):
    row = []
    for c in range(8):
        v = SCALER(blk[r*8+c], 10)
        v += 128  # unsigned
        v = max(0, min(255, v))
        row.append(v >> 4)
    print(f"  row {r}: {row}")
