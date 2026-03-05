# JIT Playground — Marco de Validación del Dynarec

## Concepto

ELF separado (`jit_playground.elf`) que linka contra los mismos módulos del dynarec
pero tiene su propio `main()`. Genera secuencias de instrucciones R3000A en RAM,
las ejecuta a través del JIT, y compara el estado resultante del CPU contra valores
esperados definidos en cada test.

**Objetivo:** Verificar que cada optimización/cambio al JIT produce código R5900
que ejecuta correctamente la semántica R3000A, sin depender de tests de juegos
(que son caja negra y lentos).

## Arquitectura

```
jit_playground.elf
├── tests/jit/playground_main.c   — Entry point, framework, test runner
├── tests/jit/playground_tests.c  — 30+ test cases usando mini-DSL
├── tests/jit/playground.h        — Header compartido, DSL macros
└── Linka con: dynarec_*.o, cpu.o, memory.o, gte.o (stubs para HW)
```

### Flujo de un test

```
1. BEGIN_TEST("add_basic")
2. SET_REG(V0, 100)           → cpu.regs[2] = 100
   SET_REG(V1, 200)           → cpu.regs[3] = 200
3. EMIT_PSX(ADDU(V0, V0, V1)) → psx_ram[test_base+0] = opcode ADDU
   EMIT_PSX(JR(RA))           → psx_ram[test_base+4] = opcode JR $ra
   EMIT_PSX(NOP())            → psx_ram[test_base+8] = NOP (delay slot)
4. RUN_JIT(1000)              → compile_block() + execute with 1000 cycles
5. EXPECT_REG(V0, 300)        → assert cpu.regs[2] == 300
6. END_TEST()                 → report PASS/FAIL
```

### Mini-DSL (macros en playground.h)

```c
// R3000A opcodes (para escribir en PSX RAM)
#define PSX_NOP()          0x00000000
#define PSX_ADDU(rd,rs,rt) MK_R(0,(rs),(rt),(rd),0,0x21)
#define PSX_ADDIU(rt,rs,i) MK_I(0x09,(rs),(rt),(i))
#define PSX_SUBU(rd,rs,rt) MK_R(0,(rs),(rt),(rd),0,0x23)
#define PSX_AND(rd,rs,rt)  MK_R(0,(rs),(rt),(rd),0,0x24)
#define PSX_OR(rd,rs,rt)   MK_R(0,(rs),(rt),(rd),0,0x25)
#define PSX_XOR(rd,rs,rt)  MK_R(0,(rs),(rt),(rd),0,0x26)
#define PSX_SLL(rd,rt,sa)  MK_R(0,0,(rt),(rd),(sa),0x00)
#define PSX_SRL(rd,rt,sa)  MK_R(0,0,(rt),(rd),(sa),0x02)
#define PSX_SRA(rd,rt,sa)  MK_R(0,0,(rt),(rd),(sa),0x03)
#define PSX_LUI(rt,imm)    MK_I(0x0F,0,(rt),(imm))
#define PSX_ORI(rt,rs,imm) MK_I(0x0D,(rs),(rt),(imm))
#define PSX_LW(rt,off,rs)  MK_I(0x23,(rs),(rt),(off))
#define PSX_SW(rt,off,rs)  MK_I(0x2B,(rs),(rt),(off))
#define PSX_BEQ(rs,rt,off) MK_I(0x04,(rs),(rt),(off))
#define PSX_BNE(rs,rt,off) MK_I(0x05,(rs),(rt),(off))
#define PSX_JR(rs)         MK_R(0,(rs),0,0,0,0x08)
#define PSX_MFHI(rd)       MK_R(0,0,0,(rd),0,0x10)
#define PSX_MFLO(rd)       MK_R(0,0,0,(rd),0,0x12)
#define PSX_MULT(rs,rt)    MK_R(0,(rs),(rt),0,0,0x18)
#define PSX_MULTU(rs,rt)   MK_R(0,(rs),(rt),0,0,0x19)
#define PSX_DIV(rs,rt)     MK_R(0,(rs),(rt),0,0,0x1A)
#define PSX_DIVU(rs,rt)    MK_R(0,(rs),(rt),0,0,0x1B)
#define PSX_SLTI(rt,rs,i)  MK_I(0x0A,(rs),(rt),(i))
#define PSX_SLTIU(rt,rs,i) MK_I(0x0B,(rs),(rt),(i))
#define PSX_SLT(rd,rs,rt)  MK_R(0,(rs),(rt),(rd),0,0x2A)
#define PSX_SLTU(rd,rs,rt) MK_R(0,(rs),(rt),(rd),0,0x2B)
// ... etc
```

### Framework

```c
// Contexto de test
typedef struct {
    const char *name;
    uint32_t *code_start;   // dónde se escriben los opcodes
    int       code_count;   // instrucciones escritas
    uint32_t  psx_pc;       // PC de inicio para este test
} TestCtx;

// Macros del framework
#define BEGIN_TEST(name) { ... reset cpu, invalidate cache, set ctx ... }
#define SET_REG(r, val)  cpu.regs[r] = (uint32_t)(val)
#define SET_HI(val)      cpu.hi = (uint32_t)(val)
#define SET_LO(val)      cpu.lo = (uint32_t)(val)
#define SET_MEM32(off, val) *(uint32_t*)(psx_ram + (off)) = (val)
#define EMIT_PSX(opcode) ctx.code_start[ctx.code_count++] = (opcode)
#define RUN_JIT(cycles)  { ... compile + execute ... }
#define EXPECT_REG(r, val) { if (cpu.regs[r] != (uint32_t)(val)) FAIL(...) }
#define EXPECT_HI(val)     { if (cpu.hi != (uint32_t)(val)) FAIL(...) }
#define EXPECT_LO(val)     { if (cpu.lo != (uint32_t)(val)) FAIL(...) }
#define EXPECT_MEM32(off, val) { ... }
#define END_TEST()       { ... report PASS, cleanup ... }
```

## Tests planificados (30+)

### ALU básico (8 tests)
1. `add_basic` — ADDU rd, rs, rt
2. `add_overflow` — ADD con overflow (debe generar excepción)
3. `sub_basic` — SUBU
4. `sub_overflow` — SUB con overflow
5. `and_or_xor` — AND, OR, XOR
6. `nor` — NOR
7. `addiu_signext` — ADDIU con signo negativo
8. `lui_ori` — LUI + ORI para cargar constante 32-bit

### Shifts (4 tests)
9. `sll_srl_sra` — SLL, SRL, SRA con varios SA
10. `sllv_srlv_srav` — Variable shifts
11. `shift_by_zero` — SA=0 (edge case)
12. `sra_sign_extend` — SRA preserva signo

### Multiply/Divide (4 tests)
13. `mult_basic` — MULT, check HI/LO
14. `multu_basic` — MULTU
15. `div_basic` — DIV, check quotient/remainder
16. `divu_basic` — DIVU

### Comparison (3 tests)
17. `slt_signed` — SLT con valores positivos y negativos
18. `sltu_unsigned` — SLTU
19. `slti_sltiu` — Immediate variants

### Load/Store (6 tests)
20. `lw_sw_basic` — LW/SW roundtrip
21. `lb_sb_signext` — LB/SB con sign extension
22. `lbu_sbu` — LBU (zero extend)
23. `lh_sh` — LH/SH halfword
24. `lwl_lwr` — Unaligned load
25. `swl_swr` — Unaligned store

### Branch (5 tests)
26. `beq_taken` — BEQ cuando rs==rt
27. `beq_not_taken` — BEQ cuando rs!=rt
28. `bne_taken` — BNE
29. `branch_delay_slot` — Instrucción en delay slot se ejecuta
30. `branch_delay_store` — Store en delay slot afecta resultado

### Interacción entre instrucciones (3+ tests)
31. `load_delay` — LW seguido de uso del registro (load delay slot)
32. `store_load_forwarding` — SW→LW a misma dirección
33. `loop_counter` — Loop con BNE + ADDIU (test de super-blocks)
34. `jal_ra` — JAL escribe $ra, JR $ra retorna

## Dependencias y stubs

El playground necesita linkar con los módulos del dynarec pero NO con GPU/SPU/CDROM.
Se necesitan stubs mínimos para:

```c
// Stubs requeridos
void WriteHardware(uint32_t addr, uint32_t data, int width) { /* no-op */ }
uint32_t ReadHardware(uint32_t addr, int width) { return 0; }
void BIOS_HLE_A(void) {}
void BIOS_HLE_B(void) {}
void BIOS_HLE_C(void) {}
void GTE_Execute(uint32_t op) {}
// + cualquier otro símbolo externo que el linker necesite
```

## Ejecución

```bash
# Build
cmake --build build --target jit_playground

# Run
make -C build run-playground
# O directamente:
perl -e 'alarm 30; exec @ARGV' make -C build run GAMEARGS=tests/jit/jit_playground.elf
```

**Output esperado:**
```
=== JIT Playground ===
[PASS] add_basic
[PASS] add_overflow
[PASS] sub_basic
...
[FAIL] branch_delay_store: $v0 expected=42 got=0
...
=== Results: 33/34 passed, 1 failed ===
```
