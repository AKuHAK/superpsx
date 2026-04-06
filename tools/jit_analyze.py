#!/usr/bin/env python3
"""
jit_analyze.py — Offline analysis of SuperPSX JIT block dumps.

Reads binary dump produced by ENABLE_JIT_DUMP and generates:
  1. Hot blocks ranked by total EE impact (exec_count × native_count)
  2. Per-block expansion ratios and instruction category breakdown
  3. Call graph edges (JAL/J targets) with weighted execution counts
  4. Summary statistics

Usage:
    python3 tools/jit_analyze.py build/jitdump.bin [--top N] [--graph out.dot] [--callees 0x80XXXXXX]
"""

import struct
import sys
import argparse
from collections import defaultdict

# --- MIPS R3000A disassembly helpers ---

REG_NAMES = [
    "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
    "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
    "$s0", "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
    "$t8", "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra",
]

OP_NAMES = {
    0x02: "J", 0x03: "JAL", 0x04: "BEQ", 0x05: "BNE",
    0x06: "BLEZ", 0x07: "BGTZ",
    0x08: "ADDI", 0x09: "ADDIU", 0x0A: "SLTI", 0x0B: "SLTIU",
    0x0C: "ANDI", 0x0D: "ORI", 0x0E: "XORI", 0x0F: "LUI",
    0x20: "LB", 0x21: "LH", 0x22: "LWL", 0x23: "LW",
    0x24: "LBU", 0x25: "LHU", 0x26: "LWR",
    0x28: "SB", 0x29: "SH", 0x2A: "SWL", 0x2B: "SW", 0x2E: "SWR",
    0x30: "LWC0", 0x32: "LWC2", 0x38: "SWC0", 0x3A: "SWC2",
}

SPECIAL_NAMES = {
    0x00: "SLL", 0x02: "SRL", 0x03: "SRA", 0x04: "SLLV", 0x06: "SRLV",
    0x07: "SRAV", 0x08: "JR", 0x09: "JALR",
    0x0C: "SYSCALL", 0x0D: "BREAK",
    0x10: "MFHI", 0x11: "MTHI", 0x12: "MFLO", 0x13: "MTLO",
    0x18: "MULT", 0x19: "MULTU", 0x1A: "DIV", 0x1B: "DIVU",
    0x20: "ADD", 0x21: "ADDU", 0x22: "SUB", 0x23: "SUBU",
    0x24: "AND", 0x25: "OR", 0x26: "XOR", 0x27: "NOR",
    0x2A: "SLT", 0x2B: "SLTU",
}

COP0_NAMES = {0x00: "MFC0", 0x04: "MTC0", 0x10: "RFE"}

# --- Instruction categorization ---

CAT_ALU = "ALU"
CAT_MULDIV = "MulDiv"
CAT_LOAD = "Load"
CAT_STORE = "Store"
CAT_BRANCH = "Branch"
CAT_COP0 = "COP0"
CAT_COP2_DATA = "COP2data"
CAT_GTE_CMD = "GTE_cmd"
CAT_OTHER = "Other"


def categorize_insn(word):
    """Categorize a PSX instruction into a high-level category."""
    op = (word >> 26) & 0x3F
    if op == 0:  # SPECIAL
        func = word & 0x3F
        if func in (0x18, 0x19, 0x1A, 0x1B):  # MULT/MULTU/DIV/DIVU
            return CAT_MULDIV
        if func in (0x08, 0x09):  # JR/JALR
            return CAT_BRANCH
        return CAT_ALU
    if op == 1:  # REGIMM (BLTZ/BGEZ/BLTZAL/BGEZAL)
        return CAT_BRANCH
    if op in (0x02, 0x03, 0x04, 0x05, 0x06, 0x07):  # J/JAL/BEQ/BNE/BLEZ/BGTZ
        return CAT_BRANCH
    if op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F):  # ALU imm
        return CAT_ALU
    if op in (0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26):  # Loads
        return CAT_LOAD
    if op == 0x32:  # LWC2
        return CAT_COP2_DATA
    if op == 0x3A:  # SWC2
        return CAT_COP2_DATA
    if op in (0x28, 0x29, 0x2A, 0x2B, 0x2E):  # Stores
        return CAT_STORE
    if op == 0x10:  # COP0
        return CAT_COP0
    if op == 0x12:  # COP2
        rs = (word >> 21) & 0x1F
        if rs in (0x00, 0x02, 0x04, 0x06):  # MFC2/CFC2/MTC2/CTC2
            return CAT_COP2_DATA
        return CAT_GTE_CMD
    return CAT_OTHER


def disasm_psx(word, pc):
    """Simple PSX MIPS disassembler — returns mnemonic string."""
    op = (word >> 26) & 0x3F
    rs = (word >> 21) & 0x1F
    rt = (word >> 16) & 0x1F
    rd = (word >> 11) & 0x1F
    sa = (word >> 6) & 0x1F
    func = word & 0x3F
    imm = word & 0xFFFF
    simm = imm if imm < 0x8000 else imm - 0x10000
    target = (word & 0x03FFFFFF) << 2

    if word == 0:
        return "nop"

    if op == 0:  # SPECIAL
        name = SPECIAL_NAMES.get(func, f"SPECIAL_{func:#04x}")
        if func in (0x00, 0x02, 0x03):  # SLL/SRL/SRA
            return f"{name} {REG_NAMES[rd]}, {REG_NAMES[rt]}, {sa}"
        if func in (0x08,):  # JR
            return f"{name} {REG_NAMES[rs]}"
        if func in (0x09,):  # JALR
            return f"{name} {REG_NAMES[rd]}, {REG_NAMES[rs]}"
        if func in (0x10, 0x12):  # MFHI/MFLO
            return f"{name} {REG_NAMES[rd]}"
        if func in (0x11, 0x13):  # MTHI/MTLO
            return f"{name} {REG_NAMES[rs]}"
        if func in (0x18, 0x19, 0x1A, 0x1B):  # MULT/DIV
            return f"{name} {REG_NAMES[rs]}, {REG_NAMES[rt]}"
        return f"{name} {REG_NAMES[rd]}, {REG_NAMES[rs]}, {REG_NAMES[rt]}"

    if op == 1:  # REGIMM
        branch_names = {0: "BLTZ", 1: "BGEZ", 16: "BLTZAL", 17: "BGEZAL"}
        name = branch_names.get(rt, f"REGIMM_{rt}")
        btarget = pc + 4 + (simm << 2)
        return f"{name} {REG_NAMES[rs]}, 0x{btarget & 0xFFFFFFFF:08X}"

    if op in (0x02, 0x03):  # J/JAL
        jtarget = (pc & 0xF0000000) | target
        name = OP_NAMES[op]
        return f"{name} 0x{jtarget:08X}"

    if op in (0x04, 0x05, 0x06, 0x07):  # BEQ/BNE/BLEZ/BGTZ
        name = OP_NAMES[op]
        btarget = pc + 4 + (simm << 2)
        if op in (0x06, 0x07):
            return f"{name} {REG_NAMES[rs]}, 0x{btarget & 0xFFFFFFFF:08X}"
        return f"{name} {REG_NAMES[rs]}, {REG_NAMES[rt]}, 0x{btarget & 0xFFFFFFFF:08X}"

    if op == 0x0F:  # LUI
        return f"LUI {REG_NAMES[rt]}, 0x{imm:04X}"

    if op in (0x08, 0x09, 0x0A, 0x0B):  # ADDI/ADDIU/SLTI/SLTIU
        name = OP_NAMES[op]
        return f"{name} {REG_NAMES[rt]}, {REG_NAMES[rs]}, {simm}"

    if op in (0x0C, 0x0D, 0x0E):  # ANDI/ORI/XORI
        name = OP_NAMES[op]
        return f"{name} {REG_NAMES[rt]}, {REG_NAMES[rs]}, 0x{imm:04X}"

    if op in OP_NAMES and op >= 0x20:  # Load/Store
        name = OP_NAMES[op]
        return f"{name} {REG_NAMES[rt]}, {simm}({REG_NAMES[rs]})"

    if op == 0x10:  # COP0
        rs_field = (word >> 21) & 0x1F
        if rs_field == 0:
            return f"MFC0 {REG_NAMES[rt]}, cop0_{rd}"
        if rs_field == 4:
            return f"MTC0 {REG_NAMES[rt]}, cop0_{rd}"
        if func == 0x10:
            return "RFE"
        return f"COP0 0x{word:08X}"

    if op == 0x12:  # COP2
        rs_field = (word >> 21) & 0x1F
        if rs_field == 0:
            return f"MFC2 {REG_NAMES[rt]}, gte_{rd}"
        if rs_field == 2:
            return f"CFC2 {REG_NAMES[rt]}, gte_ctrl_{rd}"
        if rs_field == 4:
            return f"MTC2 {REG_NAMES[rt]}, gte_{rd}"
        if rs_field == 6:
            return f"CTC2 {REG_NAMES[rt]}, gte_ctrl_{rd}"
        # GTE command
        cmd = word & 0x1FFFFFF
        return f"GTE 0x{cmd:07X}"

    name = OP_NAMES.get(op, f"OP_{op:#04x}")
    return f"{name} 0x{word:08X}"


# --- Binary dump parser ---

def parse_dump(path):
    """Parse jitdump.bin and return list of block dicts."""
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic != b"JITD":
            print(f"Error: bad magic {magic!r}, expected b'JITD'", file=sys.stderr)
            sys.exit(1)
        (block_count,) = struct.unpack("<I", f.read(4))

        blocks = []
        for _ in range(block_count):
            hdr = f.read(20)
            if len(hdr) < 20:
                break
            psx_pc, instr_count, native_count, cycle_count, exec_count = struct.unpack("<5I", hdr)

            psx_code = []
            for _ in range(instr_count):
                (w,) = struct.unpack("<I", f.read(4))
                psx_code.append(w)

            native_code = []
            for _ in range(native_count):
                (w,) = struct.unpack("<I", f.read(4))
                native_code.append(w)

            blocks.append({
                "pc": psx_pc,
                "instr_count": instr_count,
                "native_count": native_count,
                "cycle_count": cycle_count,
                "exec_count": exec_count,
                "psx_code": psx_code,
                "native_code": native_code,
            })

    return blocks


# --- Analysis functions ---

def extract_edges(block):
    """Extract call/jump edges from a block's PSX instructions."""
    edges = []
    pc = block["pc"]
    for i, word in enumerate(block["psx_code"]):
        insn_pc = pc + i * 4
        op = (word >> 26) & 0x3F
        if op == 0x03:  # JAL
            target = (insn_pc & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
            edges.append(("call", target))
        elif op == 0x02:  # J
            target = (insn_pc & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
            edges.append(("jump", target))
        elif op in (0x04, 0x05, 0x06, 0x07):  # BEQ/BNE/BLEZ/BGTZ
            imm = word & 0xFFFF
            simm = imm if imm < 0x8000 else imm - 0x10000
            target = (insn_pc + 4 + (simm << 2)) & 0xFFFFFFFF
            edges.append(("branch", target))
        elif op == 1:  # REGIMM
            imm = word & 0xFFFF
            simm = imm if imm < 0x8000 else imm - 0x10000
            target = (insn_pc + 4 + (simm << 2)) & 0xFFFFFFFF
            edges.append(("branch", target))
        elif op == 0 and (word & 0x3F) == 0x08:  # JR
            rs = (word >> 21) & 0x1F
            edges.append(("jr", rs))  # dynamic target
        elif op == 0 and (word & 0x3F) == 0x09:  # JALR
            rs = (word >> 21) & 0x1F
            edges.append(("jalr", rs))

    return edges


def print_hot_blocks(blocks, top_n=30):
    """Print top blocks by total EE impact."""
    # Impact = exec_count × native_count (total EE words executed across all runs)
    for b in blocks:
        b["ee_impact"] = b["exec_count"] * b["native_count"]
        b["ratio"] = b["native_count"] / max(b["instr_count"], 1)

    ranked = sorted(blocks, key=lambda b: b["ee_impact"], reverse=True)

    total_ee_impact = sum(b["ee_impact"] for b in blocks)
    total_exec = sum(b["exec_count"] for b in blocks)

    print(f"\n{'='*80}")
    print(f" HOT BLOCKS — Top {top_n} by EE impact (exec × native words)")
    print(f" Total blocks: {len(blocks)}, Total exec: {total_exec:,}")
    print(f" Total EE impact: {total_ee_impact:,} EE-word-executions")
    print(f"{'='*80}")
    print(f" {'#':>3} {'PC':>10} {'Exec':>10} {'PSXi':>5} {'EEw':>5} {'Ratio':>6} {'Impact':>12} {'Cum%':>6}  Categories")
    print(f" {'-'*3} {'-'*10} {'-'*10} {'-'*5} {'-'*5} {'-'*6} {'-'*12} {'-'*6}  {'-'*30}")

    cum = 0
    for i, b in enumerate(ranked[:top_n]):
        cum += b["ee_impact"]
        cum_pct = cum / max(total_ee_impact, 1) * 100

        # Category breakdown
        cats = defaultdict(int)
        for w in b["psx_code"]:
            cats[categorize_insn(w)] += 1
        cat_str = " ".join(f"{c}:{n}" for c, n in sorted(cats.items(), key=lambda x: -x[1]))

        print(f" {i+1:>3} 0x{b['pc']:08X} {b['exec_count']:>10,} {b['instr_count']:>5} {b['native_count']:>5} {b['ratio']:>5.1f}x {b['ee_impact']:>12,} {cum_pct:>5.1f}%  {cat_str}")

    print()


def print_category_summary(blocks):
    """Print aggregate stats by instruction category."""
    cat_psx = defaultdict(int)
    cat_ee_impact = defaultdict(int)
    cat_exec = defaultdict(int)

    # We need to weight by exec_count for runtime impact
    for b in blocks:
        for w in b["psx_code"]:
            cat = categorize_insn(w)
            cat_psx[cat] += 1
            cat_exec[cat] += b["exec_count"]
        # Attribute EE impact proportionally by category fractions
        cats_in_block = defaultdict(int)
        for w in b["psx_code"]:
            cats_in_block[categorize_insn(w)] += 1
        total_insns = max(len(b["psx_code"]), 1)
        for cat, cnt in cats_in_block.items():
            # Weighted EE impact ≈ (fraction of block that is this category) × total EE impact
            cat_ee_impact[cat] += int(b["ee_impact"] * cnt / total_insns)

    total_psx = sum(cat_psx.values())
    total_impact = sum(cat_ee_impact.values())

    print(f"\n{'='*60}")
    print(f" INSTRUCTION CATEGORY SUMMARY (static + weighted)")
    print(f"{'='*60}")
    print(f" {'Category':<12} {'Static':>8} {'%':>5} {'WeightedImpact':>16} {'%':>5}")
    print(f" {'-'*12} {'-'*8} {'-'*5} {'-'*16} {'-'*5}")

    for cat in sorted(cat_psx.keys(), key=lambda c: -cat_ee_impact.get(c, 0)):
        psx_n = cat_psx[cat]
        psx_pct = psx_n / max(total_psx, 1) * 100
        impact = cat_ee_impact.get(cat, 0)
        impact_pct = impact / max(total_impact, 1) * 100
        print(f" {cat:<12} {psx_n:>8,} {psx_pct:>4.1f}% {impact:>16,} {impact_pct:>4.1f}%")

    print()


def print_block_detail(blocks, pc):
    """Print detailed disassembly for a specific block."""
    matches = [b for b in blocks if b["pc"] == pc]
    if not matches:
        print(f"No block found at PC=0x{pc:08X}")
        return

    b = matches[0]
    print(f"\n{'='*60}")
    print(f" BLOCK 0x{b['pc']:08X} — {b['instr_count']} PSX insns → {b['native_count']} EE words ({b['ratio']:.1f}x)")
    print(f" Exec: {b['exec_count']:,}, Cycles: {b['cycle_count']}, EE Impact: {b['ee_impact']:,}")
    print(f"{'='*60}")
    print(f"\n PSX Code:")
    for i, w in enumerate(b["psx_code"]):
        ipc = b["pc"] + i * 4
        cat = categorize_insn(w)
        asm = disasm_psx(w, ipc)
        print(f"   {ipc:08X}: {w:08X}  {asm:<35s}  [{cat}]")

    print(f"\n EE Native ({b['native_count']} words):")
    for i in range(0, len(b["native_code"]), 8):
        chunk = b["native_code"][i:i+8]
        hex_str = " ".join(f"{w:08X}" for w in chunk)
        print(f"   +{i:4d}: {hex_str}")

    # Show edges
    edges = extract_edges(b)
    if edges:
        print(f"\n Edges:")
        for etype, target in edges:
            if isinstance(target, int) and etype in ("call", "jump", "branch"):
                print(f"   {etype:>8}: 0x{target:08X}")
            else:
                print(f"   {etype:>8}: {REG_NAMES[target] if isinstance(target, int) else target}")
    print()


def write_call_graph(blocks, filename):
    """Write DOT format call graph for graphviz."""
    # Build block PC set for filtering
    block_pcs = {b["pc"] for b in blocks}
    block_map = {b["pc"]: b for b in blocks}

    # Collect weighted edges
    edge_weights = defaultdict(int)  # (src_pc, dst_pc, type) → weight
    for b in blocks:
        edges = extract_edges(b)
        for etype, target in edges:
            if etype in ("call", "jump") and isinstance(target, int) and target in block_pcs:
                edge_weights[(b["pc"], target, etype)] += b["exec_count"]

    # Build node set (only nodes with edges)
    nodes = set()
    for (src, dst, _), w in edge_weights.items():
        if w > 0:
            nodes.add(src)
            nodes.add(dst)

    with open(filename, "w") as f:
        f.write("digraph jit_callgraph {\n")
        f.write("  rankdir=LR;\n")
        f.write("  node [shape=box, style=filled, fontsize=10];\n\n")

        # Nodes with labels
        max_exec = max((block_map[pc]["exec_count"] for pc in nodes if pc in block_map), default=1)
        for pc in sorted(nodes):
            b = block_map.get(pc)
            if not b:
                f.write(f'  "0x{pc:08X}" [label="0x{pc:08X}"];\n')
                continue
            # Color by heat (exec_count)
            heat = min(b["exec_count"] / max(max_exec, 1), 1.0)
            r = int(255 * heat)
            g = int(255 * (1 - heat))
            color = f"#{r:02X}{g:02X}40"
            label = f"0x{pc:08X}\\n{b['instr_count']}i→{b['native_count']}w ({b['ratio']:.1f}x)\\nexec:{b['exec_count']:,}"
            f.write(f'  "0x{pc:08X}" [label="{label}", fillcolor="{color}"];\n')

        f.write("\n")

        # Edges
        for (src, dst, etype), weight in sorted(edge_weights.items(), key=lambda x: -x[1]):
            if weight == 0:
                continue
            style = "solid" if etype == "call" else "dashed"
            pen = max(1, min(5, weight // max(max_exec // 5, 1)))
            f.write(f'  "0x{src:08X}" -> "0x{dst:08X}" [label="{etype}\\n{weight:,}", style={style}, penwidth={pen}];\n')

        f.write("}\n")

    print(f"[GRAPH] Wrote call graph with {len(nodes)} nodes, {len(edge_weights)} edges to {filename}")


def print_expansion_histogram(blocks):
    """Show distribution of expansion ratios across all blocks."""
    buckets = defaultdict(int)
    for b in blocks:
        if b["exec_count"] == 0:
            continue
        ratio = b["native_count"] / max(b["instr_count"], 1)
        bucket = int(ratio)
        buckets[bucket] += b["exec_count"]

    total = sum(buckets.values())
    print(f"\n{'='*50}")
    print(f" EXPANSION RATIO DISTRIBUTION (weighted by exec)")
    print(f"{'='*50}")

    for bucket in sorted(buckets.keys()):
        count = buckets[bucket]
        pct = count / max(total, 1) * 100
        bar = "#" * int(pct / 2)
        print(f"  {bucket:>3}x-{bucket+1:>3}x: {count:>10,} ({pct:>5.1f}%) {bar}")
    print()


def print_callee_analysis(blocks, target_pc):
    """Show all blocks that call a given PC, with their exec counts."""
    callers = []
    for b in blocks:
        edges = extract_edges(b)
        for etype, target in edges:
            if etype == "call" and target == target_pc:
                callers.append(b)
                break

    if not callers:
        print(f"No callers found for 0x{target_pc:08X}")
        return

    callers.sort(key=lambda b: -b["exec_count"])
    print(f"\n Callers of 0x{target_pc:08X} ({len(callers)} blocks):")
    for b in callers[:20]:
        print(f"   0x{b['pc']:08X}  exec:{b['exec_count']:>8,}  {b['instr_count']}i→{b['native_count']}w ({b['ratio']:.1f}x)")
    print()


# --- Main ---

def main():
    parser = argparse.ArgumentParser(description="Analyze SuperPSX JIT block dumps")
    parser.add_argument("dumpfile", help="Path to jitdump.bin")
    parser.add_argument("--top", type=int, default=30, help="Number of hot blocks to show (default: 30)")
    parser.add_argument("--graph", type=str, help="Output DOT file for call graph")
    parser.add_argument("--detail", type=str, help="Show detailed disassembly for block at given hex PC")
    parser.add_argument("--callees", type=str, help="Show callers of given hex PC")
    parser.add_argument("--histogram", action="store_true", help="Show expansion ratio histogram")
    args = parser.parse_args()

    blocks = parse_dump(args.dumpfile)
    print(f"Loaded {len(blocks)} blocks from {args.dumpfile}")

    # Compute derived fields
    for b in blocks:
        b["ee_impact"] = b["exec_count"] * b["native_count"]
        b["ratio"] = b["native_count"] / max(b["instr_count"], 1)

    if args.detail:
        pc = int(args.detail, 16)
        print_block_detail(blocks, pc)
        return

    if args.callees:
        pc = int(args.callees, 16)
        print_callee_analysis(blocks, pc)
        return

    # Default analysis
    print_hot_blocks(blocks, args.top)
    print_category_summary(blocks)

    if args.histogram:
        print_expansion_histogram(blocks)

    if args.graph:
        write_call_graph(blocks, args.graph)

    # Summary
    total_blocks = len(blocks)
    executed = [b for b in blocks if b["exec_count"] > 0]
    never_run = total_blocks - len(executed)
    total_psx = sum(b["instr_count"] for b in blocks)
    total_ee = sum(b["native_count"] for b in blocks)
    total_exec = sum(b["exec_count"] for b in blocks)
    avg_ratio = total_ee / max(total_psx, 1)

    print(f"\n{'='*50}")
    print(f" SUMMARY")
    print(f"{'='*50}")
    print(f" Blocks compiled:     {total_blocks:,}")
    print(f" Blocks executed:     {len(executed):,} ({never_run} never ran)")
    print(f" Total PSX insns:     {total_psx:,}")
    print(f" Total EE words:      {total_ee:,}")
    print(f" Avg expansion:       {avg_ratio:.2f}x")
    print(f" Total block execs:   {total_exec:,}")

    # Top 10 by exec count alone
    top_by_exec = sorted(executed, key=lambda b: -b["exec_count"])[:10]
    print(f"\n Hottest by exec count:")
    for b in top_by_exec:
        print(f"   0x{b['pc']:08X}  exec:{b['exec_count']:>8,}  {b['instr_count']}i→{b['native_count']}w ({b['ratio']:.1f}x)  cyc:{b['cycle_count']}")
    print()


if __name__ == "__main__":
    main()
