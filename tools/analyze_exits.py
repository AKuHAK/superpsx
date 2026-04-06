#!/usr/bin/env python3
"""Analyze block exit types and find superblock chain candidates."""
import struct, sys

data = open('build/jitdump.bin', 'rb').read()
j_count = jr_count = jal_count = beq_count = bne_count = other_count = 0
j_blocks = []
jal_blocks = []

off = 8  # skip 'JITD' magic + block count
while off < len(data):
    if off + 20 > len(data): break
    pc, num_psx, num_ee, cycle_cnt, exec_cnt = struct.unpack('<IIIII', data[off:off+20])
    off += 20
    psx_bytes = num_psx * 4
    ee_bytes = num_ee * 4
    if off + psx_bytes + ee_bytes > len(data): break
    psx_code = struct.unpack('<%dI' % num_psx, data[off:off+psx_bytes]) if num_psx else ()
    off += psx_bytes + ee_bytes
    if exec_cnt < 10: continue
    if num_psx < 2: continue
    
    last = psx_code[-2]  # branch/jump before delay slot
    op = (last >> 26) & 0x3F
    func = last & 0x3F
    
    if op == 0x02:
        j_count += 1
        target = (pc & 0xF0000000) | ((last & 0x03FFFFFF) << 2)
        j_blocks.append((exec_cnt, pc, target, num_psx, num_ee))
    elif op == 0x03:
        jal_count += 1
        target = (pc & 0xF0000000) | ((last & 0x03FFFFFF) << 2)
        jal_blocks.append((exec_cnt, pc, target, num_psx, num_ee))
    elif op == 0x00 and func == 0x08: jr_count += 1
    elif op == 0x00 and func == 0x09: jr_count += 1  # JALR
    elif op == 0x04: beq_count += 1
    elif op == 0x05: bne_count += 1
    else: other_count += 1

print(f'Block exit types (exec>=10):')
print(f'  J:      {j_count}')
print(f'  JAL:    {jal_count}')
print(f'  JR/JALR:{jr_count}')
print(f'  BEQ:    {beq_count}')
print(f'  BNE:    {bne_count}')
print(f'  Other:  {other_count}')

print(f'\nHot blocks ending with J (top 20):')
j_blocks.sort(reverse=True)
for ex, pc, tgt, ni, ne in j_blocks[:20]:
    print(f'  0x{pc:08X} ({ni:3d}i->{ne:3d}w) exec={ex:6d}  J->0x{tgt:08X}')

print(f'\nHot blocks ending with JAL (top 10):')
jal_blocks.sort(reverse=True)
for ex, pc, tgt, ni, ne in jal_blocks[:10]:
    print(f'  0x{pc:08X} ({ni:3d}i->{ne:3d}w) exec={ex:6d}  JAL->0x{tgt:08X}')
