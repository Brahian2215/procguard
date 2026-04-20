# Slice 2 — M2 Sample Store + extensión de M1

## Contexto

Slice 1 cerró con top-5 por CPU% en stdout, 16 tests verdes (ASAN+UBSAN+valgrind
limpios) y 16 ADRs. El integrador `src/main.c` hace dos scans crudos con
`sleep(1)` entre medias y pareo lineal por `pg_proc_id_t`.

Slice 2 introduce el primer componente con **estado cross-ciclo**: el Sample
Store (M2). Motivación: las métricas derivadas más allá de CPU% — tasas I/O,
histéresis de memoria, persistencia temporal de alertas de M4 — requieren ver
ventanas de N muestras consecutivas por proceso. El integrador actual con dos
arrays planos no escala a esa necesidad.

Simultáneamente, M1 se extiende con `vmrss` (memoria residente) y counters de
`/proc/[pid]/io` (rchar, wchar, read_bytes, write_bytes). Con esos campos, M3
en Slice 3+ puede calcular todas las métricas del PDF §5.3 excepto las de red
(requieren hilo inotify, Slice 4+).

La tercera pieza es el período de gracia G=10 ciclos: procesos que desaparecen
de `/proc` conservan su histórico G ciclos antes de liberar memoria, para que
alertas pendientes completen su evaluación (PDF §5.1, §5.4).

**Riesgo central** (ROADMAP §Slice 2): cambio de `pg_raw_sample_t` +
`pg_collector_init` impacta tests existentes. Fixtures de `test_collector.c`
sólo tienen `stat`; los nuevos parsers deben silent-fail sin abortar scan.

---

## Criterio de cierre del slice

Equivalente al de Slice 1 (`make valgrind` verde + binario visible):

- `make asan && make test && make lint-funclen` verdes.
- `make valgrind` verde tras el reset `make clean && make debug`.
- `./build/procguard` muestra top-5 por CPU% (como Slice 1) y el store ha
  sido ejercitado internamente (dos scans insertados, histórico consultado).
- 29 tests totales (8 collector + 8 metrics + 3 rank + 7 store + 3 tick).
- 6 ADRs nuevos registrados (017–022).
- Commit final firmado con mensaje imperativo.
- `STATE.md` y `TODO.md` actualizados.

---

## Decisiones tomadas (a registrar como ADRs antes del commit final)

- **ADR-017**: Contador de gracia G=10 vive en M2 vía `pg_store_tick(store,
  grace_cycles)` explícito. M1 permanece stateless para el lifecycle de
  buffers. Alinea con ADR-003 (M1 y M2 mutuamente ignorantes).
- **ADR-018**: VmRSS se lee de `/proc/[pid]/statm` (campo 2 = resident en
  páginas), multiplicado por `sysconf(_SC_PAGESIZE)` para bytes. Consistente
  con fixtures de CLAUDE.md §Testing. Diverge del texto literal del PDF §4.1
  por simplicidad de parsing y uniformidad con los tests existentes.
- **ADR-019**: `pg_store_get_history` usa **caller-provided buffer**. Sin
  `malloc` oculta en hot-path; el caller controla lifetime.
- **ADR-020**: Buffer de métricas globales (`/proc/stat`, `/proc/meminfo`)
  diferido a Slice 3+. Slice 2 cubre sólo buffer por proceso.
- **ADR-021**: `pg_collector_init` gana parámetro `skip_kernel_threads`.
  Heurística: `ppid == 2 || pid == 2` (kthreadd y sus hijos directos).
- **ADR-022** (política general): evolución de structs públicas del proyecto
  es **append-only** hasta que exista ABI estable (no aplica antes de Slice 7).
  Los campos nuevos quedan en 0 cuando la fuente subyacente falla o no
  existe. Aplicado inmediatamente a `pg_raw_sample_t` (Slice 2) y válido
  como regla por defecto para slices posteriores.

Defaults adoptados sin ambigüedad:
- **N = 16 samples** por proceso (configurable vía `pg_store_init`).
- main.c sigue siendo integrador temporal; se migra a usar el store como
  smoke test **silencioso** (sin output adicional; el store se valida via
  valgrind + tests, no via print).

**Thread-safety**: diferida a Slice 4+. El diseño actual (estado por entry,
sin globals mutables) deja espacio para un mutex por entry o global cuando
entren hilos; no introducir `pthread_mutex_t` en este slice.

---

## Arquitectura

### Archivos a crear

- `src/store/store.h` — API pública de M2.
- `src/store/store.c` — implementación.
- `tests/unit/test_store.c` — tests Unity con ASAN+UBSAN.

### Archivos a modificar

- `src/common/pg_types.h` — extensión aditiva de `pg_raw_sample_t`.
- `src/collector/collector.h` — nuevo tercer param de `pg_collector_init`.
- `src/collector/collector.c` — parsers `read_proc_statm`, `read_proc_io`,
  filtrado kernel threads, nuevo campo `skip_kt` en struct interna.
- `src/main.c` — actualizar llamada a `pg_collector_init` + migrar a store
  silencioso.
- `tests/unit/test_collector.c` — 2 call-sites de `pg_collector_init` +
  4 nuevos tests (vmrss populated/absent, io populated, kernel thread skip).
- `Makefile` — añadir targets `$(BUILD_DIR)/test_store` + `src/store/store.c`
  en `$(BUILD_DIR)/procguard`.
- `docs/DECISIONS.md` — ADRs 017–022.
- `docs/STATE.md` — módulos completos + próximos pasos.
- `docs/TODO.md` — nota sobre buffer global diferido + permisos de
  `/proc/[pid]/io` para usuario no-root.

### API pública de M2 (`src/store/store.h`)

```c
#ifndef PG_STORE_H
#define PG_STORE_H

#include "pg_types.h"
#include <stddef.h>

typedef struct pg_store pg_store_t; /* opaco */

/*
 * Inicializa el store con capacidad n_per_proc muestras por proceso.
 * Retorna:
 *   PG_OK         éxito (*store contiene un store inicializado)
 *   PG_ERR_PARSE  store == NULL o n_per_proc == 0
 *   PG_ERR_MEM    falló alocación interna
 */
int pg_store_init(pg_store_t **store, size_t n_per_proc);

/*
 * Inserta una muestra. Crea entry si el id no existe; empuja sobre el buffer
 * circular descartando la más antigua (FIFO) si lleno. Marca la entry
 * "vista este tick" (reset por pg_store_tick).
 *
 * Si el id fue liberado previamente por gracia vencida, un insert
 * posterior crea una entry nueva (count=0, sin histórico anterior).
 *
 * Invariante del caller: un mismo id no se inserta más de una vez entre
 * dos ticks consecutivos (el store no deduplica).
 *
 * Retorna:
 *   PG_OK         éxito
 *   PG_ERR_PARSE  store == NULL o sample == NULL
 *   PG_ERR_MEM    creación de entry falló
 */
int pg_store_insert(pg_store_t *store, const pg_raw_sample_t *sample);

/*
 * Copia hasta buf_cap muestras del histórico de id en buf (orden
 * cronológico: oldest first). *out_len recibe la cantidad real escrita
 * (<= buf_cap). Si el id no existe en el store (nunca insertado o
 * liberado por gracia), *out_len = 0 y retorna PG_OK (ausencia no es
 * error). Entries en período de gracia (marcadas como terminadas pero
 * aún no liberadas) siguen siendo accesibles.
 *
 * Retorna:
 *   PG_OK         éxito
 *   PG_ERR_PARSE  store == NULL, buf == NULL, out_len == NULL o buf_cap == 0
 */
int pg_store_get_history(const pg_store_t *store,
                         pg_proc_id_t id,
                         pg_raw_sample_t *buf, size_t buf_cap,
                         size_t *out_len);

/*
 * Avanza el ciclo: incrementa absent_cycles en cada entry que no recibió
 * insert desde el último tick; libera entries cuyo contador **excede**
 * grace_cycles. Las que sí recibieron insert resetean absent_cycles a 0
 * y su flag "vista" para el siguiente ciclo.
 *
 * Semántica fina: una entry insertada en el ciclo k-ésimo y no vuelta a
 * insertar sobrevive hasta el tick del ciclo k + grace_cycles inclusive;
 * se libera en el tick siguiente.
 *
 * Retorna:
 *   PG_OK         éxito
 *   PG_ERR_PARSE  store == NULL
 */
int pg_store_tick(pg_store_t *store, unsigned int grace_cycles);

/*
 * Libera el store y todos sus buffers. Seguro llamar con NULL (no-op).
 */
void pg_store_destroy(pg_store_t *store);

#endif /* PG_STORE_H */
```

### Estructura interna del store (`src/store/store.c`)

```c
typedef struct {
    pg_proc_id_t       id;
    pg_raw_sample_t   *samples;        /* array circular de n_per_proc slots */
    size_t             head;           /* próximo slot de escritura */
    size_t             count;          /* cuántos slots ocupados (<= n_per_proc) */
    unsigned int       absent_cycles;  /* ticks consecutivos sin insert */
    bool               seen_this_tick; /* set en insert, reset en tick */
} pg_store_entry_t;

struct pg_store {
    size_t              n_per_proc;
    pg_store_entry_t   *entries;
    size_t              n_entries;
    size_t              cap;
};
```

**Convención del buffer circular**:
- `head` = índice del próximo slot donde escribir (`0..n_per_proc-1`).
- `count` = cuántos slots válidos (`0..n_per_proc`).
- oldest (= primer elemento cronológico) = `samples[(head - count + n_per_proc) % n_per_proc]`.
- newest = `samples[(head - 1 + n_per_proc) % n_per_proc]` cuando `count > 0`.
- insert: `samples[head] = *sample; head = (head+1) % n_per_proc; count = min(count+1, n_per_proc)`.

### Extensión de `pg_raw_sample_t` (`src/common/pg_types.h`)

```c
typedef struct {
    pg_proc_id_t       id;
    char               comm[PG_COMM_MAX];
    char               state;
    long               ppid;
    int                tty_nr;
    unsigned long long utime;
    unsigned long long stime;
    unsigned long long timestamp_ms;

    /* Slice 2 — campos append-only (ADR-022).
     * Todos en 0 si la lectura del archivo correspondiente falla. */
    unsigned long long vmrss_bytes;  /* /proc/[pid]/statm campo 2 * pagesize */
    unsigned long long rchar;        /* /proc/[pid]/io */
    unsigned long long wchar;        /* /proc/[pid]/io */
    unsigned long long read_bytes;   /* /proc/[pid]/io */
    unsigned long long write_bytes;  /* /proc/[pid]/io */
} pg_raw_sample_t;
```

### Extensión de `pg_collector_init`

```c
/* Firma nueva — ADR-021 */
int pg_collector_init(pg_collector_t **col, const char *proc_base,
                      bool skip_kernel_threads);
```

Internamente, `struct pg_collector` gana un `bool skip_kt`. Se consume en el
scan loop después de parsear el sample: si `skip_kt && (sample.ppid == 2 ||
sample.id.pid == 2)`, se omite silenciosamente.

**Parser de `/proc/[pid]/statm`**: 7 enteros separados por espacios (PDF §4.1,
proc(5)):
```
size resident shared text lib data dt
```
Sólo interesa `resident` (campo 2). Parser:
```c
unsigned long long resident_pages = 0;
sscanf(buf, "%*s %llu", &resident_pages);  /* %*s salta size */
sample->vmrss_bytes = resident_pages * (unsigned long long)sysconf(_SC_PAGESIZE);
```
El supresor `%*s` (no `%*lu`) respeta MAKEFILE_GOTCHAS §1.

**Parser de `/proc/[pid]/io`**: texto multilínea con pares `clave: valor`.
Kernel añade campos nuevos en versiones futuras; parser posicional rompería.
Estrategia: iterar líneas con `strtok`/`strsep`, match por prefijo:
```c
if      (strncmp(line, "rchar: ", 7)       == 0) sscanf(line + 7,  "%llu", &s->rchar);
else if (strncmp(line, "wchar: ", 7)       == 0) sscanf(line + 7,  "%llu", &s->wchar);
else if (strncmp(line, "read_bytes: ", 12) == 0) sscanf(line + 12, "%llu", &s->read_bytes);
else if (strncmp(line, "write_bytes: ", 13)== 0) sscanf(line + 13, "%llu", &s->write_bytes);
```

**Permisos**: `/proc/[pid]/io` requiere root o mismo UID que el proceso.
Para `./build/procguard` corrido como usuario normal, la mayoría de
procesos darán EACCES → silent-fail → campos quedan en 0. No es un bug;
nota en `TODO.md` y en el README del slice.

Call-sites a actualizar (5 llamadas en 2 archivos):
- `src/main.c`: 1 llamada → pasa `false`.
- `tests/unit/test_collector.c`: cada test crea su propio collector; 5
  llamadas totales. Todas pasan `false`.

---

## Plan de sesiones

> **Paso 0 (post-aprobación, pre-código)**: copiar este plan a
> `docs/plans/slice-2.md` (ubicación canónica que espera CLAUDE.md §"Archivos
> de memoria del proyecto"). El archivo del sistema queda como espejo;
> `docs/plans/slice-2.md` es la fuente de verdad que leerán sesiones futuras.

**Política de commits**: CLAUDE.md §5 "Commits frecuentes" — un commit tras
cada tarea verde (no uno por sesión). Se esperan **10–14 commits totales** a
lo largo de las 4 sesiones. Cada commit deja el árbol verde (`make asan &&
make test-quick` mínimo; `make test + lint-funclen` al cerrar sesión).

---

### Sesión 1 — Extensión de M1: statm + io + kernel thread flag

**Objetivo**: `pg_raw_sample_t` crece con los 5 campos nuevos; el collector
los puebla best-effort; el flag `skip_kernel_threads` funciona.

**Mini-spike pre-TDD** (CLAUDE.md regla 3): snippet en `/tmp/spike_slice2.c`
(no se commitea) que prueba los formats de `sscanf` para statm e io con los
flags reales del proyecto. Dos objetivos:
1. Verificar que `"%*s %llu"` parsea correctamente el campo 2 de statm
   sin gatillar `-Werror=format=` (MAKEFILE_GOTCHAS §1).
2. Verificar que el parser línea-a-línea de `io` con `strncmp` prefix-match
   compila bajo `-Wstrict-prototypes -Wmissing-prototypes`.

Compile con: `gcc -std=c11 -Wall -Wextra -Werror -Wshadow -Wpointer-arith
-Wcast-align -Wstrict-prototypes -Wmissing-prototypes -D_GNU_SOURCE spike.c`.

**Fase A — cambio de tipos + firma (árbol verde sin nuevos tests)**:

1. Extender `pg_raw_sample_t` en `pg_types.h` — 5 campos al final.
2. Cambiar firma de `pg_collector_init` (añadir `bool skip_kernel_threads`)
   en `.h` y `.c`; en `.c`, guardar el bool en la struct interna; no usarlo
   aún en el scan loop (feature dormido).
3. Actualizar los 6 call-sites (5 en `test_collector.c`, 1 en `main.c`)
   pasando `false`.
4. `make asan && make test && make lint-funclen` → los 16 tests existentes
   siguen verdes. Commit: `prep collector for statm/io and kernel-thread flag`.

**Fase B — tests nuevos RED-GREEN**:

5. **RED**: `test_vmrss_populated_and_absent` en `test_collector.c`. Fixture:
   añadir `100/statm` con contenido `"1000 250 50 10 0 240 0\n"` (size=1000,
   resident=250); pids 200 y 300 **sin** `statm`. Assert: pid 100 tiene
   `vmrss_bytes == 250 * sysconf(_SC_PAGESIZE)`; pids 200 y 300 tienen
   `vmrss_bytes == 0`. Correr → falla.

6. **GREEN**: implementar `read_proc_statm(proc_base, pid_str, sample)`.
   Invocar desde el scan loop post-`read_proc_stat`. Silent-fail → no toca
   el sample. Commit: `read vmrss from /proc/[pid]/statm`.

7. **RED**: `test_io_counters_populated` — fixture `100/io` con:
   ```
   rchar: 1024
   wchar: 2048
   syscr: 10
   syscw: 20
   read_bytes: 512
   write_bytes: 1024
   cancelled_write_bytes: 0
   ```
   Assert: los 4 campos parseados; pid 200 (sin `io`) tiene los 4 en 0.

8. **GREEN**: implementar `read_proc_io` con prefix-match. Invocar desde
   scan loop después de `read_proc_statm`. Commit: `read io counters from
   /proc/[pid]/io`.

9. **RED**: `test_kernel_thread_filter`. Fixture extra: pid 2 (kthreadd,
   ppid=0) y pid 50 (kworker, ppid=2). Test a: init con `false` → scan
   devuelve 5 procesos (100, 200, 300, 2, 50). Test b: init con `true`
   → scan devuelve sólo los 3 originales.

10. **GREEN**: activar check `if (skip_kt && (sample.ppid == 2 ||
    sample.id.pid == 2)) continue;` en el scan loop. Commit: `filter
    kernel threads when skip_kernel_threads flag is set`.

**Cierre Sesión 1**: `make asan && make test && make lint-funclen` verdes.
Tests: 5 originales + 3 nuevos = **8 collector** + 8 metrics + 3 rank = 19.

---

### Sesión 2 — M2 Sample Store básico (insert + get_history + destroy)

**Objetivo**: `pg_store_init/insert/get_history/destroy` funcionando, sin
gracia ni tick. 1 entry por `pg_proc_id_t`, buffer circular de N=16.

**Tareas TDD**:

1. Crear `src/store/store.h` con la API completa + docstrings.
2. Crear `tests/unit/test_store.c` con `setUp/tearDown` vacíos (samples
   construidos en memoria, no hay procfs sintético para esta sesión salvo
   el test de integración del punto 10).
3. Añadir target `$(BUILD_DIR)/test_store` al Makefile + `TEST_BINS`.
   Compilar con los mismos flags ASAN+UBSAN. Commit: `scaffold store module
   with empty test binary`.

4. **RED** → **GREEN**: `test_init_destroy_clean` — `pg_store_init(&s, 16)`
   → `pg_store_destroy(s)`. Estructura `pg_store` + `pg_store_entry_t`
   según sección Arquitectura. Init alloca store vacío (entries=NULL,
   cap=0); destroy libera cada `entry->samples` + `entries` + `store`.
   Commit: `add store init and destroy`.

5. **RED** → **GREEN**: `test_insert_single_sample` — 1 insert, 1
   `get_history` → 1 muestra con datos preservados. Implementación: `insert`
   busca entry por id (lineal); si no existe, crece `entries` (realloc
   amortizado) y aloca `samples` de `n_per_proc`. Escribe usando
   convención head/count. `get_history` copia `count` samples desde
   oldest hacia newest. Commit: `add store insert and get_history`.

6. **RED** → **GREEN**: `test_buffer_wraparound_and_buf_cap` — N=3,
   insertar 5 samples, `get_history` con buf_cap=10 → `out_len == 3` con
   las 3 más recientes en orden; luego con buf_cap=2 → `out_len == 2` con
   las 2 más recientes. Cubre wrap-around y buf_cap cap simultáneamente.
   Commit: `handle circular buffer wraparound and buf_cap limits`.

7. **RED** → **GREEN**: `test_multiple_entries_independent` — 3 ids
   distintos, cada uno insertado con 2 samples; `get_history` de cada uno
   retorna exactamente sus 2 samples. Valida que la búsqueda lineal por
   `pg_proc_id_t` matchea pid **y** starttime (dos ids con mismo pid y
   distinto starttime quedan como entries separadas). Commit: `isolate
   entries by (pid, starttime)`.

8. **RED** → **GREEN**: `test_get_history_unknown_id_returns_zero` — id
   nunca insertado; assert `out_len == 0`, retorno `PG_OK`. La búsqueda
   lineal devuelve NULL → `*out_len = 0`. Commit: `return zero-length for
   unknown id in get_history`.

9. **RED** → **GREEN**: `test_null_args_return_parse_err` — un test único
   que cubre: `pg_store_init(NULL, 16)`, `pg_store_init(&s, 0)`,
   `pg_store_insert(NULL, ...)`, `pg_store_insert(s, NULL)`,
   `pg_store_get_history(NULL/buf=NULL/out_len=NULL/buf_cap=0, ...)`.
   Assert `PG_ERR_PARSE` en todos. `pg_store_destroy(NULL)` no-op.
   Commit: `validate NULL args across store API`.

10. **RED** → **GREEN**: `test_integration_scan_inserts_into_store` —
    integra M1 con M2. Crea fixture `/tmp/pg_test_proc/` mínimo (3 pids);
    `pg_collector_init(false)`, `pg_collector_scan`, iterar el array e
    insertar cada sample en el store; `get_history` por cada id retorna
    exactamente 1 sample con el `comm` correcto. Smoke test integrado.
    Commit: `add end-to-end integration test (scan → insert → history)`.

**Cierre Sesión 2**: `make asan && make test && make lint-funclen` verdes.
Tests: 19 previos + 7 store = **26 totales**.

---

### Sesión 3 — `pg_store_tick` + gracia G=10

**Objetivo**: `pg_store_tick(store, grace_cycles)` incrementa contadores
de ausencia y libera entries expiradas.

**Tareas TDD**:

1. **RED** → **GREEN**: `test_tick_grace_boundary` — insertar A;
   `tick(s, 10)` diez veces sin reinsert → `get_history(A)` sigue
   retornando el sample (absent_cycles = 10, aún dentro de gracia); tick
   número 11 → `get_history(A)` retorna `out_len == 0` (entry liberada,
   absent_cycles > 10). Implementación: iterar entries; si
   `!seen_this_tick` incrementar `absent_cycles` (liberar si
   `absent_cycles > grace_cycles`); si `seen_this_tick` resetear
   `absent_cycles = 0` y `seen_this_tick = false`. En `insert`, setear
   `seen_this_tick = true`. La liberación compacta `entries` con swap-con-
   último. Commit: `add tick with grace period expiration`.

2. **RED** → **GREEN**: `test_tick_resets_counter_on_reinsert` — insertar
   A, 5 ticks, reinsertar A, 9 ticks más → `get_history(A)` todavía
   retorna (no expiró: reset en insert de la mitad). Commit: `reset absent
   counter on reinsert`.

3. **RED** → **GREEN**: `test_tick_frees_multiple_expired` — 3 ids
   insertados; 12 ticks sin inserts → los 3 liberados (`get_history`
   retorna 0 para cada uno). ASAN valida que no hay leaks (leak detection
   es el assert pasivo). Cubre la compactación con múltiples elementos.
   Commit: `compact entries array on multi-expire`.

**Cierre Sesión 3**: `make asan && make test && make lint-funclen` verdes.
Tests: 26 previos + 3 tick = **29 totales**.

*(Los NULL-checks de `pg_store_tick` se añaden al test merged
`test_null_args_return_parse_err` creado en la Sesión 2, que ya contaba
en los 7 de store; no es un test adicional.)*

---

### Sesión 4 — Integración silenciosa en main.c + ADRs + cierre

**Objetivo**: `src/main.c` usa el store internamente; slice cerrado con
`make valgrind` verde, STATE.md actualizado, ADRs registrados.

**Tareas**:

1. Actualizar `src/main.c`: crear un store al inicio (`pg_store_init(&s,
   16)`), insertar todas las muestras del scan #1, llamar
   `pg_store_tick(s, PG_GRACE_CYCLES)`, insertar todas las del scan #2,
   `pg_store_tick` de nuevo. El output visible sigue siendo el top-5 por
   CPU% (sin diagnósticos añadidos). Al final, `pg_store_destroy(s)`.
   Manejo de errores: si `pg_store_insert` retorna ≠ `PG_OK`, fail-loud
   coherente con ADR-015 (imprimir a stderr, `goto cleanup`, `return 1`).
   Commit: `integrate sample store into slice-2 main`.

2. Actualizar Makefile: `src/store/store.c` en la regla de
   `$(BUILD_DIR)/procguard` + `-Isrc/store`. Commit: `wire store into
   procguard build`.

3. Registrar ADRs 017–022 en `docs/DECISIONS.md` siguiendo el formato de
   ADRs 014–016 (Contexto / Decisión / Consecuencias). Commit: `record
   slice-2 ADRs (017–022)`.

4. Actualizar `docs/TODO.md` con:
   - Buffer global de M2 (diferido por ADR-020; se aborda cuando M3 pleno
     en Slice 3+ consuma `/proc/stat` para CPU% de sistema).
   - Permisos de `/proc/[pid]/io`: sin privilegios, los counters saldrán
     en 0 para la mayoría de procesos. Documentar en README del slice.
   - Si algún día se necesitan campos de `/proc/[pid]/status` (UIDs real/
     efectivo para M5 Security), se añaden con un tercer parser análogo
     a `read_proc_statm`/`read_proc_io`.

5. Actualizar `docs/STATE.md` con el template obligatorio (Sesión cerrada,
   módulos completos, tests, último commit, próximos pasos → Slice 3
   M4 Alert & Governance). Commit: `update state and todo for slice-2 close`.

6. Verificación end-to-end:
   ```bash
   make asan                                  # exit 0, cero warnings
   make test                                  # 29 tests, 0 failures
   make lint-funclen                          # OK
   make clean && make debug && make valgrind  # 0 bytes lost
   ./build/procguard                          # top-5 real (sin cambios visibles)
   ./build/procguard /tmp/pg_test_proc        # opcional contra fixture
   ```

---

## Migración de tests existentes

Riesgo identificado en ROADMAP: cambio de `pg_raw_sample_t` +
`pg_collector_init`. Mitigación:

- **`pg_raw_sample_t`** (ADR-022): extensión append-only. Los 8 campos
  originales mantienen offset + tamaño. Fixtures existentes no definen
  `statm`/`io` → los campos nuevos quedan en 0. Tests existentes no los
  leen → siguen verdes sin modificación.

- **`pg_collector_init`** (ADR-021): cambio de firma forzoso. Actualizar
  los 6 call-sites (5 en `test_collector.c`, 1 en `main.c`). 1-line change
  por sitio. Verificación pre-commit: `grep -rn 'pg_collector_init' src/
  tests/` debe mostrar exactamente los 6 matches esperados tras el cambio.

---

## Verificación end-to-end

**Gates por sesión**: cada sesión cierra con `make asan && make test &&
make lint-funclen` verdes antes del commit final de la sesión. Durante la
iteración RED-GREEN usar `make test-quick` (leak detection desactivada,
MAKEFILE_GOTCHAS §6).

**Gate final de slice** (Sesión 4, paso 6):

```bash
make asan                                  # exit 0, cero warnings
make test                                  # 29 tests, 0 failures
make lint-funclen                          # OK
make clean && make debug && make valgrind  # 0 bytes in 0 blocks lost
./build/procguard                          # top-5 real
```

**ADRs registrados**: 017–022 (seis nuevos).

**Archivos nuevos**: `src/store/{store.h,store.c}`, `tests/unit/test_store.c`.

**Commits esperados**: 10–14 totales (uno por tarea verde, CLAUDE.md §5).

**Salida del slice**: árbol listo para Slice 3 (M4 Alert & Governance básico),
que consumirá `pg_store_get_history` para evaluar persistencia temporal de
alertas sobre ventanas de N samples.
