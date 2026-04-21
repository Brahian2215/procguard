# Slice 3 — Completar M3 Metrics Engine

## Objetivo

Añadir tasas I/O por segundo (`rchar`, `wchar`, `read_bytes`, `write_bytes`)
a `src/metrics/` con el mismo patrón que `pg_metrics_cpu_percent`. Decidir
explícitamente RSS (pase directo) y red (deuda técnica). Sin cambios
arquitectónicos: ~40 líneas de aritmética pura + tests.

## Estado a fecha

**Punto de partida** (Slice 2 cerrado, `f3fa99b` + `e89e0a0` + `ccc7569`):
- `pg_raw_sample_t` contiene `rchar`, `wchar`, `read_bytes`, `write_bytes`,
  `vmrss_bytes` y `timestamp_ms` — M1 ya los recolecta.
- `src/metrics/` sólo expone `pg_metrics_cpu_percent` con sentinel `-1.0f`
  y clamp `[0, 100*ncpus]` (ADR-006).
- 23 tests verdes bajo `make asan && make test && make valgrind && make lint-funclen`.

## Hallazgos a integrar antes del core

1. **ADRs huérfanos.** `metrics.h` cita ADR-010/011/012 y `pg_types.h` cita
   ADR-022, pero `DECISIONS.md` llega hasta ADR-007 tras la consolidación
   post-sanitize. Se arregla en commit `chore:` separado.
2. **Memoria** del índice queda stale; se actualiza al cerrar sesión.
3. **Red por-proceso**: `/proc/[pid]/net/dev` fuera del netns del proceso
   no da datos por-PID sin `setns` + `CAP_SYS_ADMIN`. Rompe la premisa
   "M3 = funciones puras". Se difiere explícitamente como deuda.
4. **RSS**: `vmrss_bytes` ya está en bytes. Pase directo; no amerita
   función (glue code, exento de test dedicado por CLAUDE.md §3).
5. **Funclen**: helper `static compute_rate` evita boilerplate y mantiene
   la función principal ≤50 líneas.
6. **"Tests inline no escala"**: se anota, no se refactoriza en este slice.

## Fase 0 — Housekeeping ADRs (commit `chore:`)

- [src/metrics/metrics.h](../../src/metrics/metrics.h): `ADR-011` → `ADR-006`
  (el sentinel ya existe como ADR-006).
- [src/metrics/metrics.h](../../src/metrics/metrics.h): `ADR-010` → referencia
  a **ADR-008** nuevo (inyección explícita de `hz`/`ncpus`, trade-off real
  vs. `sysconf()` global → determinismo en tests).
- [src/metrics/metrics.h](../../src/metrics/metrics.h): `ADR-012` → reemplazar
  por texto sin número (monotonía de jiffies no garantizada; guarda defensiva,
  no trade-off).
- [src/common/pg_types.h](../../src/common/pg_types.h): `ADR-022` → reemplazar
  por "append-only por convención (CLAUDE.md)".
- [docs/DECISIONS.md](../DECISIONS.md): añadir ADR-008.

## Fase 1 — Tasas I/O (TDD)

### API ([src/metrics/metrics.h](../../src/metrics/metrics.h))

```c
typedef struct {
    float rchar_per_s;
    float wchar_per_s;
    float read_bytes_per_s;
    float write_bytes_per_s;
} pg_io_rates_t;

void pg_metrics_io_rates(const pg_raw_sample_t *prev,
                         const pg_raw_sample_t *curr,
                         pg_io_rates_t *out);
```

Semántica:
- `out == NULL` → no-op (idiomático, estilo `free`).
- `prev == NULL` o `curr == NULL` o id mismatch → 4 campos a `-1.0f`.
- `elapsed_ms == 0` → 4 campos a `0.0f` (espejo CPU%).
- **Underflow por-counter**: sólo ese campo a `-1.0f`; los counters son
  independientes, descartar los 4 perdería información válida.
- Sin clamp superior (las tasas I/O no tienen techo teórico).

### Implementación ([src/metrics/metrics.c](../../src/metrics/metrics.c))

Helper `static` para respetar funclen y evitar duplicación:

```c
static float compute_rate(unsigned long long prev_c,
                          unsigned long long curr_c,
                          double elapsed_s);
```

### Tests ([tests/unit/test_metrics.c](../../tests/unit/test_metrics.c))

5 nuevos:
1. `test_io_rates_happy_mixed` — 4 counters con ratios distintos, 1 s
   elapsed, verifica cada tasa.
2. `test_io_rates_id_mismatch_sentinel_all` — pid o starttime distinto →
   4 campos a `-1.0f`.
3. `test_io_rates_null_sentinel_all` — prev/curr NULL (con `out` válido
   preinicializado) → 4 campos a `-1.0f`.
4. `test_io_rates_single_underflow` — `read_bytes` decrece, otros 3 suben
   → sólo `read_bytes_per_s` en `-1.0f`, otras 3 válidas.
5. `test_io_rates_zero_elapsed` — `timestamp_ms` idéntico → 4 campos en
   `0.0f`.

Total tras Slice 3: **28 tests** (8 collector + 10 metrics + 10 store).

## Fase 2 — RSS (cero código)

Bloque de comentario en [src/metrics/metrics.h](../../src/metrics/metrics.h)
aclarando que `vmrss_bytes` ya está en bytes y el caller lee directo del
campo de `pg_raw_sample_t`. No se crea función.

## Fase 3 — Cierre (commit `docs:`)

- [docs/STATE.md](../STATE.md): estado → 28 tests, M3 completo en métricas
  derivables; roadmap Slice 4 = M4 Alert & Governance.
- [docs/STATE.md](../STATE.md) "Deuda técnica": añadir **red por-proceso**.
- ADR-008 ya fue añadido en Fase 0.

## Flujo de commits

1. `chore: fix dangling ADR references in M3 headers` — Fase 0.
2. `test: add failing io_rates tests (RED)` — declaración + stub + 5 tests.
3. `feat(metrics): implement pg_metrics_io_rates (GREEN)` — helper + impl.
4. `docs: close slice 3 (network deferred, RSS pass-through)` — STATE.md,
   comentario RSS.

Cada commit verifica `make asan && make test` verde.

## Criterio de cierre

- 28 tests verdes bajo `make asan && make test`.
- `make lint-funclen` limpio.
- `make clean && make debug && make valgrind` sin errores ni fugas.
- STATE.md cierra Slice 3, apunta a Slice 4 (M4).
- DECISIONS.md incluye ADR-008.
- Comentarios ADR del código sincronizados con DECISIONS.md.
- Red en "Deuda técnica" explícita.

## Fuera de scope

- Red por-proceso (Slice 4+, probablemente dentro de M5 si se corre como root).
- Refactor de "tests inline no escala" (Slice 4 cuando M4 añada test binary).
- Cambios en `src/main.c` para consumir tasas I/O (lo hará el consumidor
  real, M4 Alert).

## Riesgos

- Tolerancia float en `test_io_rates_happy_mixed`: usar
  `TEST_ASSERT_FLOAT_WITHIN(0.001f, ...)` (CLAUDE.md §convenciones).
- `-Werror=format` u otros gotchas: `compute_rate` usa `double` intermedio
  para evitar pérdida de precisión con counters grandes; cast final a float
  es explícito. No hay `%lu` involucrado.
