# JIT Code Expansion — Optimization Proposals

## Current Expansion Ratios (baseline, 16 PSX insns/block)

Measured with `test_expansion.c` in the JIT playground. `native_count` includes
per-block prologue/epilogue overhead (~32 EE words).

### Baseline

| Block content | PSX | EE | Ratio |
|---|---|---|---|
| Empty (JR+NOP only) | 2 | 32 | 16.0x |
| 1× NOP | 3 | 33 | 11.0x |
| 4× NOP | 6 | 33 | 5.5x |
| 16× NOP | 18 | 33 | 1.8x |

**Prologue+epilogue = ~32 EE words fixed overhead per block.**

### ALU / Shifts / Immediates (GOOD — near optimal)

| Instruction | PSX | EE | Ratio | Notes |
|---|---|---|---|---|
| ADDU / SUBU / AND / OR / XOR / NOR | 18 | 38 | 2.1x | 1 PSX → ~1 EE + slot overhead |
| ADDIU / ANDI / ORI / XORI | 18 | 37 | 2.1x | |
| SLL / SRL / SRA | 18 | 37 | 2.1x | |
| SLLV / SRLV / SRAV | 18 | 38 | 2.1x | |
| LUI | 18 | 36 | 2.0x | |
| SLT / SLTU / SLTI / SLTIU | 18 | 37-38 | 2.1x | |
| MFHI / MFLO | 18 | 36 | 2.0x | |

**Per-instruction cost ≈ 0.3-0.4 EE words** (after subtracting prologue). Nearly 1:1.

### Multiply / Divide (HIGH — inherent complexity)

| Instruction | PSX | EE | Ratio | Notes |
|---|---|---|---|---|
| MULT / MULTU | 18 | 147 | 8.2x | ~7 EE/insn (64-bit result handling) |
| DIV | 18 | 275 | 15.3x | div-by-zero + overflow checks |
| DIVU | 18 | 243 | 13.5x | div-by-zero check only |

### Load / Store (CRITICAL — highest expansion)

| Instruction | PSX | EE | Ratio | Notes |
|---|---|---|---|---|
| LW | 18 | 420 | 23.3x | ISC + align + range + scratchpad + slow |
| LH / LHU | 18 | 420-452 | 23-25x | Same + sign-extend for LH |
| LB / LBU | 18 | 131 | 7.3x | Uses cold slow-path, simpler |
| SW | 18 | 481 | 26.7x | ISC + align + range + scratchpad + slow |
| SH | 18 | 481 | 26.7x | Same as SW |
| SB | 18 | 353 | 19.6x | |

### COP2 / GTE (HIGH — CU check + C call overhead)

| Instruction | PSX | EE | Ratio | Notes |
|---|---|---|---|---|
| MTC2 / CTC2 | 18 | 433 | 24.1x | CU2 check(9w) + lite call overhead |
| MFC2 / CFC2 | 18 | 305 | 16.9x | CU2 check + inline LW or lite call |
| COP2 RTPS | 18 | 416 | 23.1x | CU2 check + full C call |
| COP2 NCLIP | 18 | 384 | 21.3x | CU2 check + full C call |

### Mixed (realistic game patterns)

| Pattern | PSX | EE | Ratio | Notes |
|---|---|---|---|---|
| ALU chain (4 insns × 4) | 18 | 47 | 2.6x | Near-optimal |
| LW+use (2 insns × 8) | 18 | 231 | 12.8x | Memory dominates |
| GTE xform (5 insns × 3) | 17 | 424 | 24.9x | CU check + C calls |
| SW burst (4 × 4) | 18 | 484 | 26.9x | Worst case |

---

## Optimization Proposals

### P1: Hoist CU2 Check to Block Prologue (COP2 instructions: 24x → ~8x)

**Impact: HIGH (~3x reduction for COP2-heavy blocks)**
**Effort: 1-2 hours**
**Risk: Low**

Currently every COP2 instruction independently checks `SR.CU2`:
```
LW T8, SR(S0)       ; load SR
SRL T8, 30           ; extract CU2 bit
ANDI T8, 1           ; mask
BNE T8, skip         ; branch if CU2 enabled
NOP                  ; delay slot
LUI A0, pc_hi        ; exception args (2-3 words)
ORI A0, pc_lo
ADDIU A1, $0, 2
{ emit_call_c }      ; ~15-20 words (flush + JAL + reload)
skip:
```

That's **~9 words of CU2 check** per COP2 instruction (plus the emit_call_c dead code).
For a block with 5 MTC2+MFC2+COP2 instructions, that's 45+ words of repeated checks.

**Proposal:** During `block_scan()`, detect if the block contains ANY COP2 instructions.
If yes, emit ONE CU2 check in the prologue. All subsequent COP2 instructions in that
block skip the check.

```c
/* In block_scan: */
scan->has_cop2 = 0;
for (int i = 0; i < n; i++) {
    uint32_t op = code[i] >> 26;
    if (op == 0x12 || op == 0x32 || op == 0x3A) /* COP2, LWC2, SWC2 */
        scan->has_cop2 = 1;
}

/* In prologue: */
if (scan.has_cop2) {
    emit CU2 check once;
    // exception path: abort block
}

/* In COP2 handlers: */
// Skip the per-instruction CU2 check entirely
```

**Savings:** ~9 words × N(cop2_insns) - 1 = for 5 COP2 insns: 36 words saved.
Plus eliminates the `emit_call_c` dead code (15-20 words) per instruction.
For a Crash Bandicoot GTE transform block (5 COP2 insns): 433→~150 EE words.

---

### P2: Memory Fast-Path Simplification (LW/SW: 23-27x → ~8-10x)

**Impact: VERY HIGH (biggest single improvement)**
**Effort: 2-4 hours**
**Risk: Medium**

Current LW/SW emits per instruction:
1. ISC check (~6 words: LW SR, SRL, ANDI, BNE, NOP + or cached LW,BNE,NOP)
2. Address computation (ADDU base+offset)
3. Alignment check (3 words: ANDI, BNE, AND)
4. Range check (3 words: SRL, BNE + ADDU in delay)
5. Fast path (1 word: LW or SW)
6. Jump to done (2 words: B, NOP)
7. Scratchpad inline check (~8 words)
8. Cold slow path backpatch (~4 words)

Total: ~25 words per memory instruction.

**Proposal A: SMRV (Speculative Memory Region Validation)**
If base register is known to point to RAM (via SMRV tracking or const-prop),
skip the ISC check, range check, and scratchpad check. Emit only:
```
ADDU T8, base, offset   ; effective address (or ADDIU if small)
AND  T9, T8, S3         ; phys = vaddr & 0x1FFFFFFF
ADDU T9, T9, S1         ; host = base + phys
LW   rd, 0(T9)          ; direct load
```
4 words instead of 25. For aligned accesses with const base, even 3 words.

**Proposal B: Cold-path-only for ISC/scratchpad**
Move ISC check + scratchpad check entirely to the cold slow path.
The hot path becomes: address → align check → range check → fast LW/SW.
If ISC is active or address is scratchpad, the cold path handles it.
This puts ~14 words into the cold path, leaving ~11 in the hot path.

**Estimated savings:** For SW burst (4×4), 484 → ~200 EE words (2.5x improvement).

---

### P3: Inline GTE Data Transfers (MTC2/MFC2: 24x → ~3x)

**Impact: HIGH for GTE-heavy code**
**Effort: 1-2 hours**
**Risk: Low**

MFC2 already inlines most data register reads as a single `LW` from `cpu.cp2_data[]`.
But MTC2 still calls `GTE_WriteData()` via `emit_call_c_lite` for ALL registers.

**Proposal:** Inline MTC2 the same way: `SW reg, cp2_data[rd](S0)`.
Only call GTE_WriteData for registers with side-effect behavior (if any —
most GTE data registers are plain storage).

```c
/* MTC2 inline: */
emit_load_psx_reg(REG_T8, rt);
EMIT_SW(REG_T8, CPU_CP2_DATA(rd), REG_S0);
```

2 words instead of ~25 (CU check + lite call overhead).

Similarly for CTC2: most control registers can be inlined. Only FLAG
(reg 31) and a few others need special handling.

**Savings:** MTC2 from 433 → ~40 EE words per block (with hoisted CU2 check).

---

### P4: DIV/DIVU Simplification (15x → ~5x)

**Impact: Medium**
**Effort: 1 hour**
**Risk: Low**

Current DIV emits overflow check (INT_MIN / -1) and div-by-zero check with
multiple branches and special-case code.

**Proposal:**
- **DIVU:** Only needs div-by-zero check (set HI=dividend, LO=0xFFFFFFFF).
  Can be done with BEQ+NOP+DIV+MFHI+MFLO = ~8 words (vs current 15.3).
- **DIV:** Add overflow check but use EE's native behavior if possible.
  R5900 may handle INT_MIN/-1 correctly natively (check).

---

### P5: Reduce Prologue/Epilogue Overhead (32 words → ~20 words)

**Impact: Low-Medium (matters for short blocks)**
**Effort: 2-4 hours**
**Risk: Low**

Current prologue includes:
- Stack frame setup (1-2 words)
- Save callee-saved regs (8 words: S0-S7)
- Load CPU pointer, RAM base, cycles, mask (5-6 words)
- Dynamic slot loads (8 LWs max)
- Sub-block cycle accounting (2-3 words)

**Proposal:** Use PS2 `SQ/LQ` (128-bit store/load) to batch register saves/restores.
- 4× SW → 1× SQ for S0-S3 group
- 4× SW → 1× SQ for S4-S7 group
Requires 16-byte alignment of the stack frame (may need adjustment).

**Savings:** 8 words → 2 words for register save, similar for restore.

---

### P6: FlushCache Batching

**Impact: Low (1-3%)**
**Effort: 30 minutes**
**Risk: Very Low**

`FlushCache(0); FlushCache(2);` is called after every `compile_block`.
Each is an EE syscall (~50 cycles). Batch all compilations in a dispatch
cycle and flush once.

---

## Priority Matrix

| # | Proposal | Expansion Impact | Effort | Priority |
|---|---|---|---|---|
| P1 | Hoist CU2 check | COP2: 24x → 8x | 1-2h | **HIGH** |
| P2 | Memory fast-path (SMRV) | LW/SW: 23-27x → 8-10x | 2-4h | **CRITICAL** |
| P3 | Inline MTC2/MFC2 | COP2 xfer: 24x → 3x | 1-2h | **HIGH** |
| P4 | DIV simplification | DIV: 15x → 5x | 1h | Medium |
| P5 | SQ/LQ prologue | Prologue: 32 → 20w | 2-4h | Low-Medium |
| P6 | FlushCache batch | Runtime only | 30min | Low |

**Recommended execution order:** P1 → P3 → P2 → P4 → P5 → P6

P1+P3 are low-risk, high-impact, quick wins that reduce COP2 expansion by ~80%.
P2 is the highest-impact single change but requires more careful implementation.
