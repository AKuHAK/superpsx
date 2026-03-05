# SuperPSX — Propuestas de Optimización del Sistema de Texturas

## Estado de implementación (actualizado)

| Propuesta | Estado | Commit |
|---|---|---|
| E: Minimal Round-Robin | ✅ Implementada | 3fcf97b |
| B: VRAM 1:1 Direct-Map | ✅ Implementada | 3fcf97b |
| D: CLD=4 Optimization | ✅ Implementada | 3fcf97b |
| A: CLUT-on-Demand | ⏭️ Superseded por B+E | — |
| C: Direct+CLUT LRU | ⏭️ Posible mejora futura (CLUT caching) | — |

**Resultado:** GPU TexCache bajó de ~28-35% a 6.1% avg (17-25% picos) del frame.
Los picos vienen de CLUT decode (CSM1 shuffle) + round-robin uploads que se hacen
siempre (sin caching). Propuesta C sería el siguiente paso si se quiere reducir picos.

**Bug encontrado durante implementación:** `Tex_Cache_DirtyRegion` usaba `vram_page_gen[block]++`
pero `get_region_gen()` retorna MAX sobre bloques de la región. Resultado: bloque individual
< MAX → false HIT → textura stale (visible como portraits invisibles en MK2). Fix: usar
`vram_page_gen[block] = vram_gen_counter` (valor global monotónico).

---

## Resumen del Problema Actual

### Datos del profiling (MK2 gameplay)
- ~85 combinaciones únicas de (page, CLUT) por escena
- Sólo 32 slots de cache (LRU) → **~53 evictions/frame**
- Cada eviction = Upload indices (64KB, ~200μs) + Upload CLUT (512B, ~47μs) ≈ **247μs**
- **53 × 247μs ≈ 13ms/frame** — 78% del budget a 60fps

### ¿Por qué tantas combinaciones?
MK2 usa **palette swapping**: la misma texture page (datos de índices) con muchos CLUTs distintos
(personajes, animaciones, fondo). El cache actual trata cada (page, CLUT) como entry separada.

### Layout actual de GS VRAM
```
[0..4095]        PSX VRAM mirror (CT16S, 1MB) — display + 15BPP textures
[4096..12287]    32 slots PSMT8/4 (2MB) — indices de textura cacheados
[12288..13311]   32 slots CT16 (256KB) — CLUTs pareados 1:1 con slots
[13312..16383]   Libre (~750KB)
Total GS: 16384 bloques = 4MB
```

### Observación clave
De las 85 combinaciones:
- Sólo **~10 páginas de textura únicas** (datos de índices)
- Pero **~30 CLUTs distintos**
- El bottleneck es re-subir 64KB de índices cada vez sólo porque cambia la paleta de 512B

---

## Propuesta A: "CLUT-on-Demand" — Mínima Invasión

### Concepto
Mantener el cache unificado de 32 slots pero detectar cuando una eviction se puede convertir
en un **CLUT-only update** (misma page, distinto CLUT).

### Cambios

**1. Separar `combined_gen` en `page_gen` + `clut_gen`:**
```c
typedef struct {
    // ... campos existentes ...
    uint32_t page_gen;     // gen sólo de los datos de textura
    uint32_t clut_gen;     // gen sólo de los datos de CLUT
    uint32_t combined_gen; // max(page_gen, clut_gen) — para invalidación
} TexPageCacheEntry;
```

**2. En el path de MISS, buscar page_reuse antes de LRU eviction:**
```c
// Antes de eviction:
uint32_t page_only_gen = get_region_gen(tex_page_x, tex_page_y, tex_hw_w, 256);
for (int i = 0; i < TEX_CACHE_SLOTS; i++) {
    TexPageCacheEntry *e = &tex_page_cache[i];
    if (e->valid && e->tex_format == tex_format &&
        e->tex_page_x == tex_page_x && e->tex_page_y == tex_page_y &&
        e->page_gen == page_only_gen)  // ¡Page data aún válida!
    {
        // CLUT-only upload (512B en vez de 64KB)
        Upload_CLUT_CSM1(e->hw_cbp, clut_x, clut_y, tex_format);
        // Actualizar entry con nuevo CLUT
        e->packed_key = packed_key;
        e->clut_x = clut_x; e->clut_y = clut_y;
        e->clut_gen = get_region_gen(clut_x, clut_y, clut_entries, 1);
        e->combined_gen = max(e->page_gen, e->clut_gen);
        e->lru_tick = tex_cache_tick;
        tex_hash_insert(h, packed_key, i);
        *out_slot_x = e->hw_tbp0; *out_slot_y = e->hw_cbp;
        return 2;
    }
}
// Si no → eviction normal (code actual)
```

### Análisis

| Aspecto | Valor |
|---------|-------|
| Líneas de código | ~40 |
| Cambios en gpu_primitives.c | **CERO** |
| Riesgo de regresión | Bajo (misma interfaz, mismos TBP0/CBP por slot) |
| Ahorro estimado | ~37 evictions → CLUT-only (~7.4ms ahorrados) |
| Resultado | ~13ms → **~5.6ms** (reducción **57%**) |

### Limitaciones
- Los 32 slots se siguen compartiendo entre todas las combinaciones
- Si hay >32 pages únicas (poco probable), sigue habiendo evictions
- El slot cambia de "identidad" (nueva CLUT key) pero mantiene TBP0/CBP fijos

---

## Propuesta B: "VRAM 1:1 Indexado" — Sólo Paletas Fuera

### Concepto
Los datos de índices de textura (PSMT8/PSMT4) tienen **posiciones fijas y deterministas**
en la VRAM de PSX. En vez de cachearlos en slots LRU, asignar un TBP0 **fijo** a cada
posible página de textura PSX. Sólo las paletas (CLUTs) se gestionan por separado.

### Geometría de bloques GS (PSMT8)
```
PSMT8 Page = 128 × 64 pixels = 8192 bytes = 32 bloques
Una textura PSX 8BPP = 256 × 256 = 8 pages PSMT8 = 256 bloques

PSX tiene 16 posiciones X × 2 posiciones Y = 32 páginas posibles
32 páginas × 256 bloques = 8192 bloques = 2MB
```

### Alineamiento natural — ¡Las páginas PSX caen en fronteras de page!
```
PSX page_x: 0, 64, 128, ..., 960 (halfword coords)
PSMT8 texel_x = page_x × 2 = 0, 128, 256, ..., 1920
PSMT8 page_width = 128 → page_x*2 siempre múltiplo de 128 ✓

PSX page_y: 0 ó 256
PSMT8 page_height = 64 → page_y siempre múltiplo de 64 ✓
```

### Layout GS VRAM propuesto
```
[0..4095]        PSX VRAM (CT16S) — display + 15BPP               = 1.0 MB
[4096..12287]    32 PSX pages PSMT8 (determinístico)               = 2.0 MB
[12288..14335]   64 CLUT slots CT16 (round-robin)                  = 0.5 MB
[14336..16383]   Libre (~500KB)
                                                          Total    = 3.5 MB / 4.0 MB
```

### Cómputo de TBP0 (O(1), sin búsqueda)
```c
// Cada PSX page tiene un TBP0 fijo:
#define PAGE_TBP_BASE  4096
#define PAGE_STRIDE    256   // 256×256 PSMT8 = 256 bloques

static inline int page_tbp0(int page_x, int page_y) {
    int page_id = (page_x >> 6) + (page_y >> 8) * 16;  // 0..31
    return PAGE_TBP_BASE + page_id * PAGE_STRIDE;
}
```

### Dirty tracking por página
```c
// Array de 32 entries: generación de cada página PSMT8
static uint32_t psmt8_page_gen[32];  // gen cuando se subió por última vez

static inline int page_needs_upload(int page_id, uint32_t current_gen) {
    return psmt8_page_gen[page_id] != current_gen;
}

// Después de upload:
psmt8_page_gen[page_id] = current_gen;
```

### CLUT: Round-Robin (sin caching = sin bugs)
```c
#define CLUT_CBP_BASE   12288
#define CLUT_CBP_STRIDE 32      // 1 page CT16 = 32 bloques
#define CLUT_SLOTS      64
static int clut_robin = 0;

static inline int alloc_clut_cbp(void) {
    int cbp = CLUT_CBP_BASE + clut_robin * CLUT_CBP_STRIDE;
    clut_robin = (clut_robin + 1) % CLUT_SLOTS;
    return cbp;
}
```

### Flujo completo
```
Primitiva (page_x, page_y, clut_x, clut_y)
        │
        ▼
 page_id = (page_x/64) + (page_y/256)*16
 tbp0 = PAGE_TBP_BASE + page_id * 256
        │
        ├── page dirty? ─── SÍ ──→ Upload_Indexed_8BPP(tbp0, page_x, page_y)
        │                           psmt8_page_gen[page_id] = current
        │    ◄─── NO ──────────────┘
        │
        ▼
 cbp = alloc_clut_cbp()             ← SIEMPRE upload CLUT (round-robin)
 Upload_CLUT_CSM1(cbp, clut_x, clut_y, fmt)
        │
        ▼
 Return (tbp0, cbp) → TEX0 con CLD=1
```

### ¿Por qué el CLUT round-robin funciona?
1. **CLD=1** en TEX0 → GS siempre recarga CLUT buffer interno desde GS VRAM
2. Cada primitiva recibe un CBP recién escrito → datos siempre frescos
3. Al dar la vuelta (64 slots), el CLUT más viejo ya no está en uso
4. **No hay caching = no hay bugs de invalidación**
5. Costo: 512B × ~85 uploads/frame = ~43KB/frame (despreciable vs 64KB × 53 = 3.4MB actual)

### ¿Qué pasa con 4BPP?
Dos opciones:

**Opción B1: Mismos slots, formato dinámico**
- Upload como PSMT8 siempre (compatible — 4BPP data es subset de bytes)
- En TEX0 cambiar PSM a PSMT4
- **PROBLEMA**: PSMT8 y PSMT4 tienen swizzle distinto → los nibbles no cuadran

**Opción B2: Slots separados para 4BPP (recomendada)**
- 32 slots extra para PSMT4 (128 bloques cada uno en vez de 256)
- Total: 32 × 256 + 32 × 128 = 12288 bloques ← ¡Cabe justo!
- TBP0 para 4BPP: `PAGE_TBP4_BASE + page_id * 128`

Con opción B2:
```
[0..4095]         PSX VRAM CT16S                          = 1.0 MB
[4096..12287]     32 slots PSMT8 (8BPP) — 256 bloques c/u = 2.0 MB
[12288..16383]    32 slots PSMT4 (4BPP) — 128 bloques c/u = 1.0 MB
                                                   Total  = 4.0 MB ← COMPLETO
```

**Problema**: ¡no queda espacio para CLUTs!

**Solución**: Usar el espacio libre de la CT16S mirror. Las líneas y=380..511 de VRAM
generalmente no se usan para display (el framebuffer suele ser 320×240 × 2).
CLUTs se pueden ubicar en esa zona de la CT16S mirror:
```
CT16S: TBP_CLUT = bloques correspondientes a y=384..511 en CT16S
       = (384/64)*16*32 = 6*512 = 3072 → TBP 3072..4095 = 1024 bloques = 32 CLUT slots
```

O más simple: **no tener slots PSMT4 separados**. En la práctica, los juegos usan
mayoritariamente un formato. Si un page se accede como 4BPP y como 8BPP (raro), se re-sube.

### Análisis

| Aspecto | Valor |
|---------|-------|
| Líneas de código | ~120 |
| Cambios en gpu_primitives.c | Mínimos (necesita skip TEXFLUSH si page ya subida) |
| Riesgo de regresión | Medio-bajo (round-robin CLUT = siempre fresco) |
| Ahorro estimado MK2 | ~10 page uploads + 0 evictions/frame |
| Resultado | ~13ms → **~2.5ms** (reducción **80%**) |
| Complejidad de CLUT | **CERO** (round-robin, sin cache) |
| GS VRAM utilizada | ~3.5MB (con opción 4BPP/8BPP compartidos) |

### Ventajas clave
- **O(1) page lookup** — cero búsqueda, cero LRU, cero hash
- **CLUT siempre fresco** — imposible tener datos stale (round-robin)
- **Pages nunca se evictan** — slot fijo por page_id, sólo re-upload si dirty
- **Escalable** — funciona igual con 85 o 850 combinaciones (page,CLUT)
- **Elimina 90% del bandwidth** — de ~3.4MB/frame a ~350KB/frame

### El problema que resuelve
El bug de CLUT pool que tuvimos se debía a que el CLUT pool hit retornaba un CBP cuyo
contenido en GS VRAM podía estar desactualizado. Con round-robin **no hay hits** — cada
CLUT se sube siempre a un CBP nuevo. No hay state tracking de CLUT = no hay bugs.

---

## Propuesta C: "Direct-Map Pages + CLUT LRU Seguro" — Balance Óptimo

### Concepto
Combina lo mejor de A y B:
- **Pages: direct-mapped** (slot fijo por page_id, como Propuesta B)
- **CLUTs: LRU con invalidación correcta** (como Propuesta A pero con la corrección del bug)

### Diagnóstico del bug del CLUT pool anterior
El CLUT pool fallaba porque:
1. CLUT slot N contenía datos del CLUT α
2. Un nuevo CLUT γ evicta slot N → se sobreescriben datos 
3. Pero la hash table todavía tenía una entrada "CLUT α → slot N"
4. Cuando CLUT α vuelve → hash HIT → retorna CBP de slot N → ¡pero contiene γ!

**La corrección**: al evictar un CLUT slot, invalidar TODAS las hash entries que apunten a ese slot.

### Estructura
```c
#define PAGE_SLOTS 32     // fijos, uno por PSX page
#define CLUT_SLOTS 64     // LRU, con invalidación correcta
#define CLUT_HASH_SETS 32 // 2-way set-associative hash

typedef struct {
    int valid;
    int format;            // 0=4BPP, 1=8BPP
    uint32_t page_gen;     // gen de los datos en GS VRAM
    int tbp0;              // fijo: PAGE_TBP_BASE + id * stride
} PageSlot;

typedef struct {
    int valid;
    uint32_t clut_key;     // pack(clut_x, clut_y)
    uint32_t clut_gen;     // gen del CLUT data en VRAM
    int cbp;               // fijo: CLUT_CBP_BASE + id * stride
    uint32_t lru_tick;
} ClutSlot;

static PageSlot page_slots[PAGE_SLOTS];
static ClutSlot clut_slots[CLUT_SLOTS];
```

### Flujo
```
1. page_id = compute(page_x, page_y)
2. IF page_slots[page_id] dirty → upload page → update page_gen
3. clut_key = pack(clut_x, clut_y)
4. Search CLUT LRU:
   a. Hash lookup → HIT y gen match? → use CBP
   b. Linear scan → HIT y gen match? → use CBP + update hash
   c. MISS → evict LRU slot:
      - **Invalidar todas las hash entries que apunten al slot evicto**
      - Upload CLUT → return CBP
5. Return (page_tbp0, clut_cbp)
```

### Invalidación de hash en eviction
```c
static void clut_evict_slot(int slot_idx) {
    // Buscar y limpiar TODAS las hash entries que apunten a este slot
    for (int s = 0; s < CLUT_HASH_SETS; s++) {
        for (int w = 0; w < 2; w++) {
            if (clut_hash[s].slot_idx[w] == slot_idx)
                clut_hash[s].slot_idx[w] = -1;
        }
    }
    clut_slots[slot_idx].valid = 0;
}
```

### Análisis

| Aspecto | Valor |
|---------|-------|
| Líneas de código | ~200 |
| Cambios en gpu_primitives.c | Mínimos |
| Riesgo de regresión | Medio (CLUT LRU requiere testing cuidadoso) |
| Ahorro estimado MK2 | ~10 page uploads + ~30 CLUT-only |
| Resultado | ~13ms → **~2.6ms** (reducción **80%**) |
| CLUT hits | Evita re-upload de CLUT cuando no cambió (~60% hit rate) |

### Ventaja sobre B
- CLUT caching reduce uploads: de ~85 CLUTs/frame a ~30 (los que cambian)
- Ahorra ~55 × 47μs ≈ 2.6ms adicionales

### Riesgo
- El CLUT LRU es el componente que falló antes — requiere la corrección de eviction hash

---

## Propuesta D: "Optimización CLD" — Complementaria

### Concepto
Independiente de las propuestas anteriores. Usar CLD=2..5 para **evitar recargas
innecesarias del CLUT buffer interno del GS**.

### Estado actual
`CLD=1` (siempre recargar) en cada TEX0 write. Si dos primitivas consecutivas usan
el mismo CBP, el GS recarga el mismo CLUT dos veces — desperdicio.

### Propuesta
```c
// En vez de CLD=1 siempre:
want_tex0 |= (uint64_t)1 << 61;   // CLD=1

// Usar CLD=4: "Load if CBP0 ≠ CBP1, then CBP0=CBP1=CBP"
want_tex0 |= (uint64_t)4 << 61;   // CLD=4
```

Con CLD=4:
- **Primera vez** un CBP se usa: CBP ≠ CBP1 → recarga → CBP0=CBP1=CBP
- **Segunda+ vez** el mismo CBP: CBP0 == CBP1 == CBP → **NO recarga** (ahorro ~2μs)
- **Cambio de CBP**: nuevo CBP ≠ CBP1 → recarga → actualiza

### Riesgo
- **Bajo** — CLD=4/5 es el mecanismo estándar de PS2 para optimizar CLUT reloads
- Pero requiere que **nunca se sobreescriba un CBP sin cambiar la address**
- Con round-robin (Propuesta B) esto es automático: cada CLUT tiene CBP distinto
- Con CLUT LRU (Propuesta C): debe usarse con cuidado — si un CBP se reutiliza con datos
  nuevos, CLD=4 NO recargará si CBP no cambió

### Compatibilidad
| Propuesta | CLD=4 seguro? |
|-----------|---------------|
| A (CLUT-on-Demand) | ⚠️ Arriesgado — CBP no cambia cuando CLUT se sobreescribe |
| B (Round-Robin) | ✅ Seguro — CBP siempre distinto |
| C (CLUT LRU) | ⚠️ Necesita flag "needs_clut_reload" extra |

---

## Propuesta E: "Minimal Round-Robin" — Implementación más Simple

### Concepto
La implementación más barata posible: **mantener todo el cache actual pero usar CLUT
round-robin para eliminar el bug**.

### Cambio único
En el MISS path de `Decode_TexPage_Cached`, en vez de `cbp = HW_CLUT_CBP_BASE + evict_idx * stride`:

```c
static int clut_round_robin = 0;
static int clut_robin_max = 64;  // Usar 64 CBP addresses de forma circular

// En MISS:
cbp = HW_CLUT_CBP_BASE + clut_round_robin * HW_CLUT_CBP_STRIDE;
clut_round_robin = (clut_round_robin + 1) % clut_robin_max;
```

Y **TAMBIÉN en el HIT path**, siempre re-subir el CLUT:
```c
// En HIT tier 1/2/3:
Upload_CLUT_CSM1(e->hw_cbp, e->clut_x, e->clut_y, e->tex_format);
// El CBP del entry no cambia — usa el mismo que ya tenía
```

### Por qué funciona
- El HIT retorna el CBP del slot → no cambia
- Pero siempre re-sube el CLUT al mismo CBP → datos siempre frescos
- El GS recarga via CLD=1 → lee datos correctos

### Análisis

| Aspecto | Valor |
|---------|-------|
| Líneas de código | **~10** |
| Cambios en gpu_primitives.c | **CERO** |
| Riesgo de regresión | **Muy bajo** (sólo añade uploads) |
| Ahorro vs actual | Pequeño (sigue teniendo ~53 evictions) |
| Resultado | **Correctness fix** más que performance fix |

### Limitación
- NO resuelve el bottleneck de 13ms — sólo arregla el bug de CLUT
- Es un stepping stone hacia Propuesta A/B/C

---

## Tabla Comparativa

| | A: CLUT-on-Demand | B: VRAM 1:1 | C: Direct+LRU | D: CLD opt | E: Minimal RR |
|---|---|---|---|---|---|
| **Líneas** | ~40 | ~120 | ~200 | ~5 | ~10 |
| **Cambios prim** | 0 | Mínimos | Mínimos | ~2 | 0 |
| **Riesgo** | Bajo | Medio-bajo | Medio | Bajo | Muy bajo |
| **ms/frame MK2** | ~5.6 | ~2.5 | ~2.6 | N/A | ~13 (sin cambio) |
| **Reducción** | 57% | 80% | 80% | +5-10% | 0% |
| **CLUT bug** | N/A* | Eliminado | Corregido | N/A | Eliminado |
| **Escalabilidad** | Media | Alta | Alta | N/A | Baja |

\* A no tiene CLUT pool separado — hereda el cache unificado actual

## Mi Recomendación

### Path incremental:

**1. Primero: Propuesta E (Minimal Round-Robin)** — 10 mins de trabajo
   - Arregla el bug de CLUT inmediatamente
   - Permite verificar que Crash + MK2 funcionan

**2. Después: Propuesta B (VRAM 1:1)** — La inversión más grande pero el resultado óptimo
   - Pages direct-mapped = O(1), sin evictions
   - CLUT round-robin = siempre correcto
   - Elimina TODO el código de LRU, hash table, MRU — simplificación masiva
   - **80% de reducción en el bottleneck principal**
   - Compatible con CLD=4 (Propuesta D) para ahorro adicional

**3. Opcional: Propuesta D (CLD=4)** — Optimización complementaria
   - Se aplica encima de B para ahorrar ~5-10% adicional en CLUT reloads

### Path conservador:
**Propuesta A (CLUT-on-Demand)** — Menor riesgo, buen impacto (57%)
   - Si no queremos reestructurar el cache
   - Sólo añade el path de CLUT-reuse en MISS

---

## Apéndice: Geometría de Bloques GS

### Tamaños de página por formato
| PSM | Page (pixels) | Page (bytes) | Page (blocks) |
|-----|---------------|-------------|---------------|
| CT16S | 64 × 64 | 8192 | 32 |
| CT16 | 64 × 64 | 8192 | 32 |
| PSMT8 | 128 × 64 | 8192 | 32 |
| PSMT4 | 128 × 128 | 8192 | 32 |
| CT32 | 64 × 32 | 8192 | 32 |

### TBP0/TBW para PSX pages en mirror PSMT8
```
page_id = (page_x >> 6) + (page_y >> 8) * 16   // 0..31
tbp0    = 4096 + page_id * 256
tbw     = 4    // 256 PSMT8 pixels / 64 = 4

Ejemplo: page_x=64, page_y=0 → id=1 → tbp0=4352, tbw=4
Ejemplo: page_x=0, page_y=256 → id=16 → tbp0=8192, tbw=4
```

### PSMT4 (4BPP) — Si se usan slots separados
```
4BPP: 256×256 texels = 128×128 bytes in PSMT4 = 4 pages = 128 bloques
tbp4  = 12288 + page_id * 128
tbw   = 4  (256 PSMT4 texels / 64 = 4)
```

### CLUT CBP (round-robin, 64 slots)
```
cbp   = CLUT_BASE + robin_idx * 32
robin_idx = (robin_idx + 1) % 64
```

### Nota sobre swizzle
- **Cada PSM tiene su propio block/column swizzle** dentro de una page
- Datos subidos como PSMT8 (DPSM=PSMT8 en BITBLTBUF) se almacenan con swizzle PSMT8
- Leer con PSM=PSMT8 → GS des-swizzle PSMT8 → datos correctos ✓
- Leer datos CT16S como PSMT8 → **MAL** — swizzles incompatibles ✗
- Por eso se necesita copia separada (no se puede leer del mirror CT16S directamente)
