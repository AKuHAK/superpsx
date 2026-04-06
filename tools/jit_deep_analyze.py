#!/usr/bin/env python3
"""
jit_deep_analyze.py — Deep offline analysis of JIT dump for optimization discovery.

Detects patterns, estimates savings, identifies superblock candidates,
and provides concrete optimization recommendations.

Usage:
    python3 tools/jit_deep_analyze.py build/jitdump.bin
"""

import struct
import sys
from collections import defaultdict

# --- Re-use dump parser from jit_analyze.py ---

def parse_dump(path):
    with open(path, "rb") as f:
        magic = f.read(4)
        assert magic == b"JITD", f"Bad magic: {magic!r}"
        (block_count,) = struct.unpack("<I", f.read(4))
        blocks = []
        for _ in range(block_count):
            hdr = f.read(20)
            if len(hdr) < 20:
                break
            psx_pc, instr_count, native_count, cycle_count, exec_count = struct.unpack("<5I", hdr)
            psx_code = list(struct.unpack(f"<{instr_count}I", f.read(instr_count * 4)))
            native_code = list(struct.unpack(f"<{native_count}I", f.read(native_count * 4)))
            blocks.append({
                "pc": psx_pc, "instr_count": instr_count,
                "native_count": native_count, "cycle_count": cycle_count,
                "exec_count": exec_count, "psx_code": psx_code,
                "native_code": native_code,
            })
    return blocks


REG_NAMES = [
    "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
    "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
    "$s0", "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
    "$t8", "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra",
]

def OP(w):   return (w >> 26) & 0x3F
def RS(w):   return (w >> 21) & 0x1F
def RT(w):   return (w >> 16) & 0x1F
def RD(w):   return (w >> 11) & 0x1F
def FUNC(w): return w & 0x3F
def IMM(w):  return w & 0xFFFF
def SIMM(w):
    v = w & 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v
def JTARGET(w, pc): return (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)


PROLOGUE_WORDS = 22  # Known prologue size (compile_block preamble)


# ===================================================================
#  ANALYSIS 1: Prologue Overhead Impact
# ===================================================================

def analyze_prologue_overhead(blocks):
    """Estimate how much of total EE execution is prologue/epilogue overhead."""
    print("\n" + "=" * 70)
    print(" ANALYSIS 1: PROLOGUE/EPILOGUE OVERHEAD")
    print("=" * 70)

    total_ee_impact = 0
    total_prologue_impact = 0
    # Epilogue ~6 words (restore callee-saved + jr $ra + nop)
    EPILOGUE_WORDS = 6

    # Group by block size
    buckets = defaultdict(lambda: {"count": 0, "exec": 0, "ee_impact": 0, "overhead_impact": 0})

    for b in blocks:
        if b["exec_count"] == 0:
            continue
        impact = b["exec_count"] * b["native_count"]
        overhead = b["exec_count"] * (PROLOGUE_WORDS + EPILOGUE_WORDS)
        total_ee_impact += impact
        total_prologue_impact += overhead

        size_bucket = min(b["instr_count"], 50)
        bk = buckets[size_bucket]
        bk["count"] += 1
        bk["exec"] += b["exec_count"]
        bk["ee_impact"] += impact
        bk["overhead_impact"] += overhead

    print(f"\n Total EE impact:       {total_ee_impact:>14,}")
    print(f" Prologue+Epi impact:  {total_prologue_impact:>14,}  ({total_prologue_impact/max(total_ee_impact,1)*100:.1f}%)")
    print(f"\n Small blocks dominate overhead:")
    print(f" {'PSXi':>5} {'Blocks':>7} {'TotalExec':>12} {'Overhead%':>10}")

    for sz in sorted(buckets.keys()):
        bk = buckets[sz]
        if bk["ee_impact"] > 0:
            ovh_pct = bk["overhead_impact"] / bk["ee_impact"] * 100
            print(f" {sz:>5} {bk['count']:>7} {bk['exec']:>12,} {ovh_pct:>9.1f}%")


# ===================================================================
#  ANALYSIS 2: Pattern Detection
# ===================================================================

def detect_block_patterns(blocks):
    """Classify blocks into known patterns with optimization potential."""
    print("\n" + "=" * 70)
    print(" ANALYSIS 2: BLOCK PATTERN DETECTION")
    print("=" * 70)

    patterns = {
        "simple_increment": [],     # Load, increment, store, return
        "polling_loop": [],         # Load status, test, branch to self
        "exception_dispatch": [],   # LUI+ADDIU+JR (exception vectors)
        "leaf_function": [],        # No JAL, ends with JR $ra
        "call_heavy": [],           # Multiple JAL instructions
        "memory_copy": [],          # Load/store pairs (memcpy-like)
        "gte_compute": [],          # Heavy GTE command usage
        "tiny_block": [],           # ≤5 PSX instructions
    }

    for b in blocks:
        if b["exec_count"] == 0:
            continue
        code = b["psx_code"]
        n = len(code)
        pc = b["pc"]

        # Tiny block (≤5 insns)
        if n <= 5:
            patterns["tiny_block"].append(b)

        # Exception dispatch: LUI+ADDIU+JR sequence
        if n <= 5 and any(OP(w) == 0x0F for w in code):  # Has LUI
            jr_found = any(OP(w) == 0 and FUNC(w) == 0x08 for w in code)
            if jr_found:
                patterns["exception_dispatch"].append(b)
                continue

        # Simple increment: LUI+ADDIU to form addr, LW, ADDIU +1, JR, SW
        has_lw = any(OP(w) == 0x23 for w in code)
        has_sw = any(OP(w) == 0x2B for w in code)
        has_jr_ra = any(OP(w) == 0 and FUNC(w) == 0x08 and RS(w) == 31 for w in code)
        has_jal = any(OP(w) == 0x03 for w in code)
        if n <= 10 and has_lw and has_sw and has_jr_ra and not has_jal:
            # Check for increment pattern
            has_addiu_1 = any(OP(w) == 0x09 and SIMM(w) == 1 for w in code)
            if has_addiu_1:
                patterns["simple_increment"].append(b)
                continue

        # Polling loop: LHU/LBU from I/O, ANDI, BEQ/BNE back
        if n <= 8:
            has_io_load = False
            has_branch_back = False
            for i, w in enumerate(code):
                if OP(w) in (0x25, 0x24, 0x21, 0x23):  # LHU/LBU/LH/LW
                    base = RS(w)
                    # I/O if base is $s1 or known I/O pattern
                    has_io_load = True
                if OP(w) in (0x04, 0x05):  # BEQ/BNE
                    offset = SIMM(w)
                    target = pc + (i + 1) * 4 + offset * 4
                    if target >= pc and target < pc + n * 4:
                        has_branch_back = True
            if has_io_load and has_branch_back:
                patterns["polling_loop"].append(b)
                continue

        # Leaf function (no JAL, ends with JR $ra)
        if has_jr_ra and not has_jal:
            patterns["leaf_function"].append(b)

        # Call-heavy (2+ JALs)
        jal_count = sum(1 for w in code if OP(w) == 0x03)
        if jal_count >= 2:
            patterns["call_heavy"].append(b)

        # GTE compute (has GTE commands)
        gte_cmds = sum(1 for w in code if OP(w) == 0x12 and (w >> 21) & 0x1F >= 0x10)
        if gte_cmds >= 1:
            patterns["gte_compute"].append(b)

        # Memory copy pattern (interleaved LW/SW)
        lw_count = sum(1 for w in code if OP(w) == 0x23)
        sw_count = sum(1 for w in code if OP(w) == 0x2B)
        if lw_count >= 3 and sw_count >= 3 and abs(lw_count - sw_count) <= 2:
            patterns["memory_copy"].append(b)

    print()
    for pat_name, pat_blocks in sorted(patterns.items(), key=lambda x: -sum(b["exec_count"] * b["native_count"] for b in x[1])):
        total_exec = sum(b["exec_count"] for b in pat_blocks)
        total_impact = sum(b["exec_count"] * b["native_count"] for b in pat_blocks)
        avg_ratio = sum(b["native_count"] for b in pat_blocks) / max(sum(b["instr_count"] for b in pat_blocks), 1)
        print(f" {pat_name:<22} {len(pat_blocks):>5} blocks, {total_exec:>10,} exec, {total_impact:>12,} impact, avg {avg_ratio:.1f}x")
        # Show top 3 blocks in pattern
        for bb in sorted(pat_blocks, key=lambda x: -x["exec_count"] * x["native_count"])[:3]:
            ratio = bb["native_count"] / max(bb["instr_count"], 1)
            print(f"   0x{bb['pc']:08X}  {bb['instr_count']:>3}i→{bb['native_count']:>4}w ({ratio:.1f}x)  exec:{bb['exec_count']:>8,}")


# ===================================================================
#  ANALYSIS 3: Superblock Candidates (frequently chained blocks)
# ===================================================================

def analyze_superblock_candidates(blocks):
    """Find block sequences that chain frequently and could be merged."""
    print("\n" + "=" * 70)
    print(" ANALYSIS 3: SUPERBLOCK / TRACE CANDIDATES")
    print("=" * 70)

    block_map = {b["pc"]: b for b in blocks}
    end_pc_map = {b["pc"]: b["pc"] + b["instr_count"] * 4 for b in blocks}

    # Find J (jump) and fall-through edges between compiled blocks
    edges = defaultdict(int)  # (src_pc, dst_pc) → weight

    for b in blocks:
        if b["exec_count"] == 0:
            continue
        code = b["psx_code"]
        pc = b["pc"]

        # Block can chain to its successor (fall-through)
        fall_through_pc = pc + len(code) * 4
        if fall_through_pc in block_map:
            # Not all blocks fall through — need to check if last insn is unconditional jump
            last = code[-1] if code else 0
            second_last = code[-2] if len(code) >= 2 else 0
            # If second-to-last is J/JR (delay slot is last), no fall-through
            is_unconditional_exit = (OP(second_last) == 0x02 or  # J
                                     (OP(second_last) == 0 and FUNC(second_last) == 0x08))  # JR
            if not is_unconditional_exit:
                edges[(pc, fall_through_pc)] += b["exec_count"]

        # J targets
        for i, w in enumerate(code):
            ipc = pc + i * 4
            if OP(w) == 0x02:  # J
                target = JTARGET(w, ipc)
                if target in block_map:
                    edges[(pc, target)] += b["exec_count"]
            elif OP(w) in (0x04, 0x05, 0x06, 0x07):  # BEQ/BNE/BLEZ/BGTZ
                target = (ipc + 4 + SIMM(w) * 4) & 0xFFFFFFFF
                if target in block_map:
                    edges[(pc, target)] += b["exec_count"]
            elif OP(w) == 0x03:  # JAL
                target = JTARGET(w, ipc)
                if target in block_map:
                    edges[(pc, target)] += b["exec_count"]

    # Find hot chains: sequences of blocks that execute together frequently
    print(f"\n Top 20 hot edges (block→block, weighted by exec):")
    print(f" {'From':>10} → {'To':>10}  {'Weight':>10}  {'SrcI':>5} {'DstI':>5} {'Combined EEw':>12}  {'SavedOverhead':>14}")

    OVERHEAD_PER_BOUNDARY = 28  # prologue(22) + epilogue(6) saved per merge

    ranked_edges = sorted(edges.items(), key=lambda x: -x[1])[:20]
    for (src, dst), weight in ranked_edges:
        src_b = block_map.get(src)
        dst_b = block_map.get(dst)
        if not src_b or not dst_b:
            continue
        combined_ee = src_b["native_count"] + dst_b["native_count"]
        saved = weight * OVERHEAD_PER_BOUNDARY
        print(f" 0x{src:08X} → 0x{dst:08X}  {weight:>10,}  {src_b['instr_count']:>5} {dst_b['instr_count']:>5} {combined_ee:>12}  {saved:>14,}")

    # Multi-block traces: find chains of length 3+
    print(f"\n Hot traces (3+ blocks, sorted by total weight):")

    # BFS to find chains
    traces = []
    visited_starts = set()
    for (src, dst), weight in sorted(edges.items(), key=lambda x: -x[1]):
        if weight < 1000 or src in visited_starts:
            continue
        # Build trace greedily
        trace = [src]
        current = dst
        while current in block_map and len(trace) < 10:
            trace.append(current)
            # Find heaviest outgoing edge
            best_next = None
            best_w = 0
            for (s, d), w in edges.items():
                if s == current and w > best_w and d not in trace:
                    best_next = d
                    best_w = w
            if best_next is None or best_w < weight * 0.3:
                break
            current = best_next
        if len(trace) >= 3:
            traces.append((trace, weight))
            visited_starts.update(trace)

    for trace, weight in sorted(traces, key=lambda x: -x[1])[:10]:
        total_psx = sum(block_map[pc]["instr_count"] for pc in trace if pc in block_map)
        total_ee = sum(block_map[pc]["native_count"] for pc in trace if pc in block_map)
        saved_overhead = weight * OVERHEAD_PER_BOUNDARY * (len(trace) - 1)
        pcs = " → ".join(f"0x{pc:08X}" for pc in trace)
        print(f"  [{len(trace)} blocks] weight={weight:,}  {total_psx}i→{total_ee}w  overhead saved: {saved_overhead:,}")
        print(f"    {pcs}")


# ===================================================================
#  ANALYSIS 4: Register Pressure & Slot Utilization
# ===================================================================

def analyze_register_pressure(blocks):
    """Estimate how many unique PSX registers each hot block uses."""
    print("\n" + "=" * 70)
    print(" ANALYSIS 4: REGISTER PRESSURE IN HOT BLOCKS")
    print("=" * 70)

    PINNED = {28, 29, 30, 31}  # $gp, $sp, $fp, $ra

    hot = sorted(blocks, key=lambda b: -b["exec_count"] * b["native_count"])[:30]

    print(f"\n {'PC':>10} {'PSXi':>5} {'EEw':>5} {'Ratio':>6} {'UniqueRegs':>10} {'Pinned':>7} {'Dynamic':>8} {'Spill?':>6}")
    for b in hot:
        regs_read = set()
        regs_written = set()
        for w in b["psx_code"]:
            op = OP(w)
            if op == 0:  # SPECIAL
                regs_read.add(RS(w))
                regs_read.add(RT(w))
                regs_written.add(RD(w))
            elif op in (0x04, 0x05):  # BEQ/BNE
                regs_read.add(RS(w))
                regs_read.add(RT(w))
            elif op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E):  # ALU imm
                regs_read.add(RS(w))
                regs_written.add(RT(w))
            elif op == 0x0F:  # LUI
                regs_written.add(RT(w))
            elif op in (0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26):  # Loads
                regs_read.add(RS(w))
                regs_written.add(RT(w))
            elif op in (0x28, 0x29, 0x2A, 0x2B, 0x2E):  # Stores
                regs_read.add(RS(w))
                regs_read.add(RT(w))
            elif op == 0x12:  # COP2
                rs_f = (w >> 21) & 0x1F
                if rs_f in (0, 2):  # MFC2/CFC2
                    regs_written.add(RT(w))
                elif rs_f in (4, 6):  # MTC2/CTC2
                    regs_read.add(RT(w))

        all_regs = (regs_read | regs_written) - {0}  # exclude $zero
        pinned_used = all_regs & PINNED
        dynamic_needed = all_regs - PINNED
        spills = len(dynamic_needed) > 8  # DYN_SLOT_COUNT = 8

        ratio = b["native_count"] / max(b["instr_count"], 1)
        print(f" 0x{b['pc']:08X} {b['instr_count']:>5} {b['native_count']:>5} {ratio:>5.1f}x {len(all_regs):>10} {len(pinned_used):>7} {len(dynamic_needed):>8} {'YES' if spills else 'no':>6}")


# ===================================================================
#  ANALYSIS 5: Memory Access Locality
# ===================================================================

def analyze_memory_locality(blocks):
    """Find blocks with multiple loads/stores to same base register — base caching opportunities."""
    print("\n" + "=" * 70)
    print(" ANALYSIS 5: MEMORY ACCESS BASE REUSE")
    print("=" * 70)

    total_reuse_impact = 0
    reuse_blocks = []

    for b in blocks:
        if b["exec_count"] == 0:
            continue
        # Count loads/stores per base register
        base_accesses = defaultdict(int)
        for w in b["psx_code"]:
            op = OP(w)
            if op in (0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,  # Loads
                      0x28, 0x29, 0x2A, 0x2B, 0x2E,  # Stores
                      0x32, 0x3A):  # LWC2/SWC2
                base = RS(w)
                base_accesses[base] += 1

        # Find bases with 2+ accesses
        reusable = {base: cnt for base, cnt in base_accesses.items() if cnt >= 2}
        if reusable:
            max_reuse = max(reusable.values())
            total_reuse_accesses = sum(v - 1 for v in reusable.values())  # Savings = re-accesses
            # Each re-access saves ~2 words (AND mask + ADDU base) if base already computed
            savings_per_exec = total_reuse_accesses * 2
            total_savings = savings_per_exec * b["exec_count"]
            total_reuse_impact += total_savings
            reuse_blocks.append({
                "block": b,
                "reusable": reusable,
                "savings_per_exec": savings_per_exec,
                "total_savings": total_savings,
            })

    reuse_blocks.sort(key=lambda x: -x["total_savings"])

    total_ee_impact = sum(b["exec_count"] * b["native_count"] for b in blocks if b["exec_count"] > 0)
    print(f"\n Total EE impact:           {total_ee_impact:>14,}")
    print(f" Total base-reuse savings:  {total_reuse_impact:>14,} ({total_reuse_impact/max(total_ee_impact,1)*100:.1f}%)")
    print(f"\n Top 15 blocks with base reuse opportunities:")
    print(f" {'PC':>10} {'Exec':>8} {'PSXi':>5} {'Bases':>40}  {'Save/exec':>10} {'TotalSave':>12}")

    for item in reuse_blocks[:15]:
        b = item["block"]
        bases_str = ", ".join(f"{REG_NAMES[base]}×{cnt}" for base, cnt in sorted(item["reusable"].items(), key=lambda x: -x[1]))
        print(f" 0x{b['pc']:08X} {b['exec_count']:>8,} {b['instr_count']:>5} {bases_str:>40}  {item['savings_per_exec']:>10} {item['total_savings']:>12,}")


# ===================================================================
#  ANALYSIS 6: Branch Overhead
# ===================================================================

def analyze_branch_overhead(blocks):
    """Estimate EE words spent on branch handling vs actual computation."""
    print("\n" + "=" * 70)
    print(" ANALYSIS 6: BRANCH OVERHEAD ESTIMATION")
    print("=" * 70)

    # Each PSX branch typically generates:
    #  - ~8-12 EE words for taken path (flush slots, set PC, BEQ/BNE, epilogue)
    #  - ~4-6 EE words for fall-through (just patch target)
    # vs ALU: ~2-3 EE words, Load: ~6-8 EE words, Store: ~7-9 EE words

    BRANCH_EE_COST = 12  # Estimated average EE words per PSX branch
    ALU_EE_COST = 2
    LOAD_EE_COST = 6
    STORE_EE_COST = 8

    total_branch_cost = 0
    total_block_exec_impact = 0

    for b in blocks:
        if b["exec_count"] == 0:
            continue
        branches = sum(1 for w in b["psx_code"] if OP(w) in (0x02, 0x03, 0x04, 0x05, 0x06, 0x07) or
                       (OP(w) == 0 and FUNC(w) in (0x08, 0x09)) or
                       OP(w) == 1)
        total_branch_cost += branches * BRANCH_EE_COST * b["exec_count"]
        total_block_exec_impact += b["exec_count"] * b["native_count"]

    print(f"\n Estimated branch handling:  {total_branch_cost:>14,} EE-word-execs")
    print(f" Total EE impact:           {total_block_exec_impact:>14,}")
    print(f" Branch overhead share:     {total_branch_cost/max(total_block_exec_impact,1)*100:.1f}%")


# ===================================================================
#  ANALYSIS 7: Optimization Recommendations
# ===================================================================

def generate_recommendations(blocks):
    """Generate concrete, prioritized optimization recommendations."""
    print("\n" + "=" * 70)
    print(" OPTIMIZATION RECOMMENDATIONS (sorted by estimated impact)")
    print("=" * 70)

    block_map = {b["pc"]: b for b in blocks}
    total_ee_impact = sum(b["exec_count"] * b["native_count"] for b in blocks if b["exec_count"] > 0)

    recommendations = []

    # R1: Tiny block prologue reduction
    tiny_overhead = 0
    tiny_count = 0
    for b in blocks:
        if b["exec_count"] > 0 and b["instr_count"] <= 5:
            # These blocks are mostly prologue. A light prologue saving 10 words would help
            tiny_overhead += b["exec_count"] * 10  # Save ~10 words per execution
            tiny_count += 1
    if tiny_overhead > 0:
        recommendations.append({
            "name": "Light prologue for tiny blocks (≤5 PSX insns)",
            "impact": tiny_overhead,
            "pct": tiny_overhead / max(total_ee_impact, 1) * 100,
            "detail": f"{tiny_count} blocks affected. Reduce prologue from 22→12 words for blocks that only use a few callee-saved regs. "
                      f"Currently $s0-$s7+$fp are saved unconditionally; tiny blocks typically need only $s0(cpu), $s1(ram), $s2(cycles), $s3(mask).",
        })

    # R2: Exception vector fast path
    exc_blocks = [b for b in blocks if b["pc"] in (0x80000080,) and b["exec_count"] > 0]
    exc_overhead = sum(b["exec_count"] * (b["native_count"] - 4) for b in exc_blocks)
    if exc_overhead > 0:
        recommendations.append({
            "name": "Exception vector hardcoded dispatch",
            "impact": exc_overhead,
            "pct": exc_overhead / max(total_ee_impact, 1) * 100,
            "detail": f"Block 0x80000080 executes {exc_blocks[0]['exec_count']:,} times, always jumping to same handler (0x0C80). "
                      f"Hardcode this as a direct J target in the trampoline instead of compiling a full block.",
        })

    # R3: Block chaining prologue elimination
    # Estimate: for DBL-linked blocks, if we could skip prologue on chained entry
    # ~28 words saved per chain boundary × chain frequency
    chain_savings = 0
    for b in blocks:
        if b["exec_count"] == 0:
            continue
        # Blocks entered via DBL skip prologue, but blocks entered via dispatch don't
        # Estimate: 30% of block executions are via dispatch (need prologue)
        # With smarter chaining, we could reduce this further
        chain_savings += int(b["exec_count"] * 0.3 * 22)
    if chain_savings > 0:
        recommendations.append({
            "name": "Reduce dispatch-entry prologue overhead",
            "impact": chain_savings,
            "pct": chain_savings / max(total_ee_impact, 1) * 100,
            "detail": "~30% of block entries go through full dispatch (hash table miss → recompile/lookup). "
                      "A lightweight JR dispatch that skips prologue for same-context blocks could save 22 words per dispatch entry.",
        })

    # R4: Const-address load/store specialization
    # Blocks using LUI+ADDIU to form a constant address and then LW/SW through it
    const_addr_savings = 0
    const_blocks = 0
    for b in blocks:
        if b["exec_count"] == 0:
            continue
        code = b["psx_code"]
        # Look for LUI followed by ADDIU/ORI, then LW/SW using that register
        for i in range(len(code) - 2):
            if OP(code[i]) == 0x0F:  # LUI
                rt = RT(code[i])
                # Next instruction modifies same reg
                if OP(code[i + 1]) in (0x09, 0x0D) and RS(code[i + 1]) == rt and RT(code[i + 1]) == rt:
                    # Check if subsequent instructions use this as a base
                    for j in range(i + 2, min(i + 6, len(code))):
                        if OP(code[j]) in (0x23, 0x2B, 0x20, 0x24, 0x25, 0x28, 0x29) and RS(code[j]) == rt:
                            # Const address access! Could be hardcoded
                            const_addr_savings += b["exec_count"] * 2  # Save ~2 words (LUI+ADDIU → direct)
                            const_blocks += 1
                            break
    if const_addr_savings > 0:
        recommendations.append({
            "name": "Const-address memory access hardcoding",
            "impact": const_addr_savings,
            "pct": const_addr_savings / max(total_ee_impact, 1) * 100,
            "detail": f"{const_blocks} LUI+ADDIU+LW/SW sequences. Fold constant address at compile time → emit direct host address load.",
        })

    # R5: Memory base caching across consecutive accesses
    base_cache_savings = 0
    for b in blocks:
        if b["exec_count"] == 0:
            continue
        base_accesses = defaultdict(int)
        for w in b["psx_code"]:
            op = OP(w)
            if op in (0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
                      0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x32, 0x3A):
                base_accesses[RS(w)] += 1
        for base, cnt in base_accesses.items():
            if cnt >= 2:
                base_cache_savings += (cnt - 1) * 2 * b["exec_count"]  # 2 words per reuse

    if base_cache_savings > 0:
        recommendations.append({
            "name": "Memory base address caching (AND+ADDU reuse)",
            "impact": base_cache_savings,
            "pct": base_cache_savings / max(total_ee_impact, 1) * 100,
            "detail": "When multiple L/S use same base register, compute host address once and reuse. "
                      "Saves AND(mask)+ADDU(base) = 2 words per reuse.",
        })

    # R6: Polling loop fast-forward
    poll_savings = 0
    poll_blocks = 0
    for b in blocks:
        if b["exec_count"] < 100:
            continue
        code = b["psx_code"]
        if len(code) <= 6:
            has_load = any(OP(w) in (0x25, 0x24, 0x21, 0x23) for w in code)
            has_branch_back = False
            for i, w in enumerate(code):
                if OP(w) in (0x04, 0x05):
                    target_off = SIMM(w)
                    if target_off < 0:
                        has_branch_back = True
            if has_load and has_branch_back:
                poll_savings += b["exec_count"] * b["native_count"] * 0.8  # 80% of executions could be skipped
                poll_blocks += 1

    if poll_savings > 0:
        recommendations.append({
            "name": "Polling loop fast-forward / yield",
            "impact": int(poll_savings),
            "pct": poll_savings / max(total_ee_impact, 1) * 100,
            "detail": f"{poll_blocks} polling loops detected. When I/O status hasn't changed between iterations, "
                      f"fast-forward cycles instead of re-executing the loop body.",
        })

    # Sort by impact and print
    recommendations.sort(key=lambda r: -r["impact"])
    print()
    for i, r in enumerate(recommendations):
        print(f" {i+1}. {r['name']}")
        print(f"    Estimated saving: {r['impact']:,} EE-word-execs ({r['pct']:.1f}% of total)")
        print(f"    {r['detail']}")
        print()


# ===================================================================
#  Main
# ===================================================================

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <jitdump.bin>")
        sys.exit(1)

    blocks = parse_dump(sys.argv[1])
    print(f"Loaded {len(blocks)} blocks")

    for b in blocks:
        b["ee_impact"] = b["exec_count"] * b["native_count"]
        b["ratio"] = b["native_count"] / max(b["instr_count"], 1)

    analyze_prologue_overhead(blocks)
    detect_block_patterns(blocks)
    analyze_superblock_candidates(blocks)
    analyze_register_pressure(blocks)
    analyze_memory_locality(blocks)
    analyze_branch_overhead(blocks)
    generate_recommendations(blocks)


if __name__ == "__main__":
    main()
