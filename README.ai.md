# AI / Copilot Context — SuperPSX

PSX emulator running **natively on PS2** (EE/R5900 ~294MHz) via MIPS→MIPS JIT recompiler.
Codebase: ~20K lines C99 (5.5K dynarec, 4.7K GPU, 2K GTE, rest subsystems).

## Build & Test

See `.github/copilot-instructions.md` for exact commands with macOS-safe timeouts.

```bash
# Build
cmake -S . -B build && cmake --build build

# GTE test (expect: 1150 passed, 0 failed)
# CPU test (expect: 0 errors — grep -c "error")
# Timer test (expect: completes without hang)
# Crash Bandicoot + MK2 (manual visual test)
```

**IMPORTANT:** macOS has no `timeout`; use `perl -e 'alarm N; exec @ARGV'`.
Always redirect to file (`> /tmp/out.txt 2>&1`), never pipe — pipes cause SIGPIPE.

## CMake Options

| Flag | Default | Description |
|---|---|---|
| `ENABLE_PSX_TLB` | OFF | TLB fast-path for RAM (experimental, known bugs) |
| `ENABLE_VRAM_DUMP` | OFF | VRAM dumping (reduces performance) |
| `ENABLE_HOST_LOG` | ON | Host logging |
| `ENABLE_DEBUG_LOG` | ON | Debug logging |
| `ENABLE_STUCK_DETECTION` | ON | Stuck detection |
| `ENABLE_PROFILING` | OFF | gprof instrumentation |
| `ENABLE_LTO` | OFF | Link-Time Optimization |
| `ENABLE_DYNAREC_STATS` | OFF | Dynarec execution statistics |
| `ENABLE_SUBSYSTEM_PROFILER` | ON | Per-subsystem wall-clock profiler (12 categories) |
| `ENABLE_TEX_DEBUG` | OFF | Texture debug overlay (colored bounding boxes + printf) |
| `HEADLESS` | OFF | Build without GPU (no-op stubs) |

## Architecture

### JIT Register Map (10 pinned PSX → EE registers + 4 infrastructure)

| PSX Reg | EE Reg | Convention |
|---|---|---|
| $v0 (2) | T3 | caller-saved |
| $v1 (3) | T4 | caller-saved |
| $a0 (4) | T5 | caller-saved |
| $a1 (5) | T6 | caller-saved |
| $a2 (6) | T7 | caller-saved |
| $s0 (16) | S6 | callee-saved |
| $s1 (17) | S7 | callee-saved |
| $gp (28) | FP | callee-saved |
| $sp (29) | S4 | callee-saved |
| $ra (31) | S5 | callee-saved |

**Infrastructure:** S0=cpu ptr, S1=RAM base, S2=cycles_left, S3=mask(0x1FFFFFFF)
**Dynamic slots:** T0, T1, T2 — frequency-based per-block assignment, **write-through** (every store also writes cpu.regs[])
**Scratch:** T8, T9 (with scratch cache), AT

### Dynarec Module Layout (5.5K lines)

| File | Lines | Purpose |
|---|---|---|
| `dynarec_compile.c` | 1369 | Block compiler, prologue/epilogue, DCE pre-scan, super-blocks |
| `dynarec_insn.c` | 1402 | Per-instruction emission (ALU, loads, stores, COP0/COP2) |
| `dynarec_memory.c` | 1049 | Memory access (range checks, cold paths, TLB backpatch) |
| `dynarec_run.c` | 1030 | Block dispatch loop, trampoline setup, hash table |
| `dynarec_emit.c` | 657 | Low-level emitters, pinned reg sync, C call trampolines |
| `dynarec_cache.c` | 243 | Block cache, SMC page tracking, direct block linking |
| `dynarec.h` | 489 | Shared header, EMIT macros, MK_R/MK_I/MK_J |

### GPU Subsystem (4.7K lines)

| File | Lines | Purpose |
|---|---|---|
| `gpu_primitives.c` | 1366 | PSX GP0 primitive → PS2 GS translation, lazy state tracking |
| `gpu_texture.c` | 958 | CLUT texture decode, direct-map page cache, HW CLUT uploads |
| `gpu_commands.c` | 848 | GP0/GP1 command processing, VRAM transfers |
| `gpu_vram.c` | 436 | VRAM upload/transfer, shadow VRAM → GS |
| `gpu_core.c` | 382 | GS environment setup, FRAME/DISPLAY registers |
| `gpu_dma.c` | 317 | GPU DMA controller, linked-list traversal |
| `gpu_gif.c` | 138 | GIF buffer management, DMA send to GS |
| `gpu_stub.c` | 287 | Headless GPU stubs |

### GS VRAM Layout (4MB = 16384 blocks)

```
[0..4095]      PSX VRAM CT16S mirror (1MB) — display + 15BPP textures
[4096..12287]  32 format-agnostic page slots PSMT8/4 (2MB) — direct-mapped
[12288..14335] 64 CLUT round-robin CT16 slots (512KB)
[14336..16383] Free (512KB)
```

Texture cache: O(1) lookup via `page_id = (x>>6) + (y>>8)<<4`, TBP0 = 4096 + id*256.
CLUT: always uploaded fresh to next robin slot (no caching = no staleness).
CLD=4 in TEX0: GS skips redundant CLUT reloads when CBP unchanged.

### Code Buffer Trampoline Layout

| Offset | Size | Trampoline |
|---|---|---|
| [0] | 2 words | Slow-path (JR RA) |
| [2] | 28 words | Abort/exit (flush pinned + restore callee-saved) |
| [32] | 36 words | Full C-call (flush/reload all 10 pinned regs) |
| [68] | 24 words | Lite C-call (flush/reload 5 caller-saved pinned) |
| [96] | ~30 words | Jump dispatch (JR/JALR inline hash lookup) |
| [128] | 10 words | Memory slow-path (save RA, store PC, call lite) |
| [144+] | ... | JIT compiled blocks |

### Key Subsystems

- **GTE:** Inline dispatch for all 22 commands + VU0 macro mode (RTPS/RTPT/MVMVA/lighting)
- **GPU:** GIF-based rendering, direct-map texture cache, HW CLUT via GS indexed modes
- **Memory:** Range-check routing (RAM→fast, non-RAM→C helpers). TLB experimental (OFF).
- **Scheduler:** Event-based timing, cycle-accurate timer/VBlank/CD-ROM scheduling
- **SPU:** Batch ADSR optimization, non-blocking audio via audsrv
- **Profiler:** 12-category wall-clock profiler (JIT, GPU, SPU, etc.) with per-frame CSV output

## Optimization History

| Commit | Optimization | Impact |
|---|---|---|
| c5adc2d | GTE inline all COP2 ops + VRAM memcpy + SMC fix | +15-25% |
| cdf3a2d | VU0 RTPS/RTPT matrix multiply | +0.7-1.0% |
| fb61e84 | VU0 MVMVA (all 3 matrices) | geometry-heavy |
| bdb8725 | VU0 light pipeline dispatch | geometry-heavy |
| 4257e8e | SPU batch ADSR | -93.5% SPU time |
| 397a7a5 | Dead Code Elimination (backward liveness) | ~5-10% |
| 59e3a94 | Pin PSX $gp to EE $fp (10 pinned regs total) | reduced LW/SW |
| b77a980 | Dynamic register slots (T0/T1/T2, write-through) | reduced LW/SW |
| 0aa22e8 | SMRV — skip range check for known-RAM base regs | ~5-8% |
| 4dcdcef | Ultra-lite trampoline (stack save/restore T3-T7) | reduced flush |
| 13f9470 | Shadow VRAM upload (eliminate 1MB readback buffer) | GPU correctness |
| 3fcf97b | Direct-map texture cache + CLUT round-robin + DirtyRegion fix | GPU correctness + perf |
| 7447029 | Revert partial dirty tracking (restore write-through) | correctness |

## Performance Profile (Crash Bandicoot, recent)

| Category | % of Frame | Notes |
|---|---|---|
| JIT Execution | 75.1% | Main bottleneck |
| GPU TexCache | 6.1% avg, 17-25% peaks | Heavy in texture-rich scenes |
| GPU Primitives | 5.4% | GP0→GS translation |
| GPU GIF Flush | 4.2% | DMA to GS |
| SPU | ~2% | Batch ADSR |
| Other | ~7% | Scheduler, SIO, CDROM |

**Overall speed:** 55.3% (30.1ms/frame vs 16.6ms target at 60fps).

## Next Steps (Roadmap)

See `docs/jit_optimization_roadmap.md` for detailed analysis.

**Priority areas:**
1. JIT optimizations (75% of frame time) — SMRV refinement, FlushCache batching, LQ/SQ bulk ops
2. GPU texture cache (up to 25% in peaks) — CLUT caching to reduce round-robin re-uploads
3. Long-term: full dynamic register allocation

## Communication

- **User speaks Spanish.** Use `ask_questions` tool for all communication.
- Always run GTE (1150/0) + CPU (0 errors) tests before committing.
- For GPU/rendering changes, ask user to test Crash Bandicoot + MK2 manually.
- Never commit docs/ files unless explicitly asked.
