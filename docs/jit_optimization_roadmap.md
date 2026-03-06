# JIT Optimization Roadmap — superpsx

## Estado actual (lo que ya tenemos)

| # | Optimización | Estado | Detalles |
|---|---|---|---|
| 1 | Register Pinning | ✅ 4 pinned + 4 infra | gp→S6, sp→S4, fp→S7, ra→S5. Infra: S0=cpu, S1=RAM, S2=cycles, S3=mask |
| 2 | Dynamic Slots (T0-T7) | ✅ 8 slots + dirty writeback | Frequency-based `dyn_assign_slots()`. **Dirty writeback**: only modified slots are flushed to `cpu.regs[]` at sync points via `dyn_dirty_mask`. All 7 flush sites use dirty-only mode. |
| 3 | Constant Propagation | ✅ | `vregs[32]` con `dirty_const_mask`, lazy materialization |
| 4 | Dead Code Elimination | ✅ | Backward liveness en `block_scan()`, ventana 64 insn (`dce_dead_mask`) |
| 5 | Direct Block Linking | ✅ | J-based DBL con back-patching + SMC check (page_gen + hash) |
| 6 | Inline Hash Dispatch | ✅ | JR/JALR, 2-way set-associative `jit_ht[4096]`, ~20 insn trampoline |
| 7 | Cold Slow Paths | ✅ | 256-entry `cold_queue[]`, deferred al final del bloque |
| 8 | Const-Address Fast Paths | ✅ | RAM, scratchpad, I_STAT, I_MASK, GPU_ReadStatus — todos los anchos (B/H/W) |
| 9 | GTE Inline + VU0 | ✅ | 22 `GTE_Inline_*` wrappers + VU0 macro mode (config `gte_use_vu0`) |
| 10 | SMC Detection | ✅ | `jit_page_gen[512]` + djb2 hash + `jit_smc_handler` + inline NULL check |
| 11 | Idle Loop Detection | ✅ | Side-effect scan, `be->is_idle` flag, cycle skipping |
| 12 | Scratch Register Cache | ✅ | T8/T9 cached (`t8_cached_psx_reg`/`t9_cached_psx_reg`), evita LW redundantes |
| 13 | Per-Instruction Cycles | 🔄 Parcial | `r3000a_cycle_cost()` + `emit_cycle_offset`. Cold/abort paths correctos. Branch epilogue usa block total |
| 14 | SPU Batch ADSR | ✅ | Tight batch loop para volumen constante, skip per-sample tick |
| 15 | Super-Blocks (Fall-Through) | ✅ | `MAX_CONTINUATIONS=3`, `MAX_SUPER_INSNS=200`. Conditional branch fall-through inline + deferred taken-path |
| 16 | Ultra-Lite Trampoline | ✅ | Stack save/restore T0-T7 en `code_buffer[68]`. Zero dependency en cpu struct |
| 17 | TLB Backpatching | ✅ | 3-insn fast path (`and,addu,lw/sw`). `TLB_Backpatch()` + `tlb_patch_emit_all()` |
| 18 | Mem Slow-Path Trampoline | ✅ | `code_buffer[128]`: partial cycle accounting (`partial_block_cycles`) |
| 19 | BIOS HLE Native Injection | ✅ | A0/B0/C0 hooks compiled inline en bloques |
| 20 | Branch/Load Delay Slots | ✅ | `in_delay_slot` tracking + `pending_load_reg/apply_now` |
| 21 | CU Exception Dirty Mask Fix | ✅ | save/restore `dyn_dirty_mask` around conditional `emit_call_c(Helper_CU_Exception)` in 9 COP handler sites |

---

## Optimizaciones pendientes (ordenadas por impacto — data de expansion_optimization_proposals.md)

### 1. Hoist CU2 Check to Block Prologue
**Impacto:** Alto (COP2: 24x → ~8x) · **Esfuerzo:** 1-2 horas · **Riesgo:** Bajo
**Estado actual:** ❌ No empezado

Every COP2 instruction independently checks SR.CU2 (~9 words + emit_call_c dead code).
Hoist to prologue: one check per block, skip all per-instruction checks.
For 5 COP2 insns: eliminates ~45 words of redundant checks.

---

### 2. Inline MTC2/MFC2 Data Transfers
**Impacto:** Alto (COP2 xfer: 24x → ~3x combinado con P1) · **Esfuerzo:** 1-2 horas · **Riesgo:** Bajo
**Estado actual:** ❌ No empezado

MFC2 already inlines most reads. MTC2 still calls GTE_WriteData via lite trampoline.
Most GTE data registers are plain storage — inline as `SW reg, cp2_data[rd](S0)`.

---

### 3. SMRV Memory Fast-Path
**Impacto:** Muy Alto (LW/SW: 23-27x → ~8-10x) · **Esfuerzo:** 2-4 horas · **Riesgo:** Medio
**Estado actual:** ❌ No empezado

Current LW/SW emits ~25 words per instruction (ISC + align + range + scratchpad + slow).
With SMRV (base reg known RAM), skip ISC/range/scratchpad: 4 words instead of 25.

---

### 4. DIV Simplification
**Impacto:** Medio (DIV: 15x → ~5x) · **Esfuerzo:** 1 hora · **Riesgo:** Bajo
**Estado actual:** ❌ No empezado

Simplify div-by-zero and overflow checks. DIVU only needs BEQ+NOP+DIV+MFHI+MFLO.

---

### 5. SQ/LQ Prologue/Epilogue
**Impacto:** Bajo-Medio (32 → ~20 words per block) · **Esfuerzo:** 2-4 horas · **Riesgo:** Bajo
**Estado actual:** ❌ No empezado

Use PS2 128-bit SQ/LQ to batch register saves/restores (4 SW → 1 SQ).

---

### 6. FlushCache Batching
**Impacto:** Bajo (1-3%) · **Esfuerzo:** 30 min · **Riesgo:** Muy Bajo
**Estado actual:** ❌ No empezado

Batch FlushCache calls across multiple compile_block invocations.

---

## Code Smells / Mejoras menores

| Issue | Descripción | Impacto |
|---|---|---|
| Prologue 26 words | 104 bytes overhead. Super-blocks mitigan (3 continuations = 1 prologue). | Bajo |
| Branch cond en stack | Trade-off por pin $gp. +1 SW/LW por conditional branch. | Aceptable |
| memset en buffer reset | `compile_block` hace memset de hasta 4MB. Innecesario. | Bajo |
| DCE ventana 64 insn | `uint64_t` bitmask. Expandible a 128 con `__uint128_t`. | Bajo |
| DMA linked-list 100K | Safety counter puede truncar display lists legítimas. | Medio |

---

## Prioridad recomendada

```
1. Hoist CU2 check to prologue   (1-2h — COP2: 24x → 8x)
2. Inline MTC2/MFC2 transfers    (1-2h — COP2 xfer: 24x → 3x)
3. SMRV memory fast-path          (2-4h — LW/SW: 23-27x → 8-10x)
4. DIV simplification              (1h — DIV: 15x → 5x)
5. SQ/LQ prologue/epilogue        (2-4h — prologue: 32 → 20 words)
6. FlushCache batching             (30 min — runtime only)
```

**Expansion ratio data:** see `docs/expansion_optimization_proposals.md` and
`tests/jit/test_expansion.c` (playground compile-only measurement).

---

## Profile: Crash Bandicoot (post texture cache overhaul)

Measured after direct-map texture cache + CLUT round-robin (commit 3fcf97b).

| Categoría | % del frame | Notas |
|---|---|---|
| JIT Execution | 75.1% | Principal bottleneck — incluye todo el R3000A emulado |
| GPU TexCache | 6.1% avg, 17-25% picos | Decode PSMT8/4 + CLUT upload. Picos en escenas con muchas texturas |
| GPU Primitives | 5.4% | Traducción GP0 → GS |
| GPU GIF Flush | 4.2% | DMA batch al GS |
| SPU | ~2% | Batch ADSR optimizado |
| Otros | ~7% | Scheduler, SIO, CDROM, etc. |

**Velocidad general:** 55.3% (30.1ms/frame vs 16.6ms target @ 60fps).

### Fases de ejecución (detalle)

| Fase | Speed | Bottleneck | JIT% | GPU TexCache% |
|---|---|---|---|---|
| BIOS Boot | 45% | JIT (75-98%) | 95% | — |
| Game Init | 87% | JIT (93%) | 93% | — |
| Logo/Menú | **100%** | Balanced | 60% | <1% |
| Level Loading | 38% | JIT (96%) | 96% | — |
| **3D Gameplay** | **34-55%** | **JIT + GPU TexCache** | 50-75% | **6-25%** |

**Hotspot #1:** `PC=80034504` (kernel idle/scheduler) con 8-23M cycles.
**JIT compilation:** <0.1% en steady-state.

### Comparación antes/después del texture cache overhaul

El cambio de LRU hash-based a direct-map eliminó búsqueda lineal y redujo el overhead
promedio de TexCache. Los picos siguen altos porque CLUT decode + CSM1 shuffle son
operaciones inherentemente costosas (256 entries × byte shuffle + GS upload).
