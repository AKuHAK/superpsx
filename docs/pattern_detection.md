# Pattern Detection / Idiom Recognition for JIT

## Overview

Detect common instruction patterns at compile time and replace with optimized native code.
SuperPSX has a unique advantage: MIPS32→R5900 width expansion allows replacing 32-bit
SW/LW loops with 128-bit SQ/LQ (4x throughput). No other MIPS emulator can do this.

## Research

Only PCSX2 does compile-time pattern detection (timeout loops). DuckStation, PPSSPP, Dolphin
do not implement general idiom recognition in their JITs. Runtime idle loop detection is
common — SuperPSX already has `poll_detect_pc`.

## Patterns

### P-IDLE: Compile-time Timeout Loop Skip
```mips
loop: ADDIU $reg, $reg, -1    # or -N
      BNE   $reg, $zero, loop
      NOP
```
**Detection**: Block scan phase, ≤4 instructions + backward BNE.
**Replacement**: `cycles += reg * cost_per_iter; reg = 0;` (skip entire loop)
**Platform**: Both PS2 and PSP (no platform-specific instructions needed)
**Impact**: 10-100x per occurrence; common in BIOS boot, menu screens, busy waits.
**PCSX2 reference**: `recSkipTimeoutLoop()` in iR5900.cpp.

### P-ZERO: Zero-Fill with SQ (PS2 only)
```mips
loop: SW    $zero, 0($base)
      ADDIU $base, $base, 4
      BNE   $base, $limit, loop
      NOP
```
**Detection**: `SW $zero, 0(rs)` + `ADDIU rs, rs, 4` + backward BNE.
**Replacement** (PS2):
```mips
# Align base to 16 bytes (handle 0-3 word remainder first)
# Main loop:
loop: SQ $zero, 0($base_ee)      # 128-bit store (16 bytes)
      ADDIU $base_ee, $base_ee, 16
      BNE $base_ee, $limit_aligned, loop
      NOP
# Handle tail remainder
```
**PSP fallback**: Keep original SW loop (PSP has no SQ).
**Impact**: 4x throughput on aligned loops. BIOS zeroes 200KB+ during boot.
**Requirements**: Base must be 16-byte aligned for SQ. Handle alignment + remainder.

### P-COPY: memcpy with LQ/SQ (PS2 only)
```mips
loop: LW    $tmp, 0($src)
      SW    $tmp, 0($dst)
      ADDIU $src, $src, 4
      ADDIU $dst, $dst, 4
      BNE   $src, $limit, loop
      NOP
```
**Detection**: LW+SW pair with matching base increments + backward BNE.
**Replacement** (PS2): LQ/SQ pair (128-bit load/store).
**PSP fallback**: Keep original loop.
**Impact**: 4x throughput on aligned copies.
**Complexity**: High — need to verify no src/dst aliasing, both 16-byte-aligned.

### P-FILL: Value-Fill with PCPYLD+SQ (PS2 only)
```mips
loop: SW    $val, 0($base)
      ADDIU $base, $base, 4
      BNE   $base, $limit, loop
      NOP
```
**Detection**: Same as P-ZERO but with non-zero source register.
**Replacement**: Broadcast $val to 128 bits via `PCPYLD v, v, v` then SQ.
**PSP fallback**: Keep original loop.
**Impact**: 4x throughput. Less common than zero-fill.

## Implementation Plan

### Phase 1: P-IDLE (both platforms)
1. Add `detect_timeout_loop()` to block scan in `dynarec_compile.c`
2. If pattern matches: emit skip-ahead math, skip block compilation
3. Guard with `#ifdef ENABLE_PATTERN_IDLE` (CMake option)
4. Test: BIOS boot time, Crash Bandicoot menu, timer tests

### Phase 2: P-ZERO (PS2 only, `#ifdef __R5900__`)
1. Add `detect_zero_fill()` to block scan
2. Emit SQ-based loop with alignment prologue/epilogue
3. Handle remainder (count % 4) with individual SW
4. Test: BIOS boot, memory clear sequences

### Phase 3: P-COPY (PS2 only, `#ifdef __R5900__`)
1. Add `detect_memcpy()` to block scan
2. Emit LQ/SQ pair loop with alignment handling
3. Test: DMA buffer copies, VRAM transfers

## Risks
- **False positives**: Pattern might match non-loop code (mitigated by backward branch check)
- **Self-modifying code**: Loop body might be overwritten during execution (mitigated by SMC
  invalidation — pattern-compiled blocks get invalidated like any other)
- **Cycle accuracy**: Skipping loop iterations changes cycle timing. Acceptable for timeout
  loops (already done at runtime). For memset/memcpy, preserve total cycle count.
- **Alignment**: SQ requires 16-byte alignment. Runtime alignment check needed.
