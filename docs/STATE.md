# Estado del Proyecto

## Última actualización
Sesión 3 — 2026-04-19 — Slice 1 cerrado: main integrador + `make valgrind` verde

## Módulos completados
- M1 Data Collector (parcial): `pg_collector_init/scan/destroy` con procfs path
  configurable. Parser de `/proc/[pid]/stat` robusto a `comm` con paréntesis
  internos. ASAN limpio (0 leaks). Pendiente para slices futuros: `vmrss`
  (Slice 2), gracia G=10 ciclos para procesos desaparecidos (Slice 2+),
  filtrado de kernel threads (Slice 2 con TUI).
- M3 Metrics Engine (parcial): `pg_metrics_cpu_percent(prev, curr, hz, ncpus)`
  como función pura stateless con clamp a `[0, 100*ncpus]`, sentinel `-1.0f`
  para NULL args / ID mismatch / underflow (ADRs 010–013). Pendiente para
  slices futuros: tasas I/O y red (Slice 2+), RSS trend (Slice 2+).
- `src/util/rank` (aux): `ranked_t` + `pg_rank_cmp_cpu_desc` (ADR-016).
  Módulo puro testeable, sobrevive a la reescritura de Slice 2 (top-N por
  CPU% será la vista principal del TUI M6).
- Integrador `src/main.c` (Slice 1, temporal): dos scans separados por
  `sleep(1)`, pareo por `pg_proc_id_t`, top-5 descendente por CPU% a stdout.
  Filtra sentinels (ADR-014), fail-loud en errores de scan (ADR-015).

## Tests pasando
- `tests/unit/test_collector.c` — 5 tests, 5 passed, 0 failed
  - `test_scan_finds_all_three`
  - `test_disappeared_process`
  - `test_recycled_pid`
  - `test_malformed_stat_is_skipped`
  - `test_parses_comm_with_internal_parens`
- `tests/unit/test_metrics.c` — 8 tests, 8 passed, 0 failed
  - `test_idle_process`
  - `test_one_percent`
  - `test_recycled_pid_starttime_differs`
  - `test_recycled_pid_pid_differs`
  - `test_full_cpu_saturates_clamp`
  - `test_zero_elapsed`
  - `test_null_args`
  - `test_underflow_returns_sentinel`
- `tests/unit/test_rank.c` — 3 tests, 3 passed, 0 failed
  - `test_orders_descending`
  - `test_orders_fractional_differences`
  - `test_equal_cpu_returns_zero`

## Estado del build
- `make asan` — exit 0, cero warnings bajo `-Wall -Wextra -Werror -Wshadow
  -Wpointer-arith -Wcast-align -Wstrict-prototypes -Wmissing-prototypes`.
- `make test` — verde (16 tests totales: 5 collector + 8 metrics + 3 rank).
- `make lint-funclen` — OK (todas las funciones <= 50 líneas).
- `make valgrind` — exit 0, `0 bytes in 0 blocks lost`, allocs == frees.
  Requiere build no-ASAN (`make clean && make debug` antes): ASan interpone
  su propio malloc y no convive con memcheck en el mismo binario. UBSan por
  sí solo sí convive, pero nuestro `CFLAGS_ASAN` activa ambos.

## Última acción ejecutada
add slice-1 main: two-scan cpu top-5 integrator (4dad16e)

## Próximos pasos
1. Slice 2 / Sesión 1: diseño de M2 Sample Store (`pg_store_init/insert/
   get_history/destroy`) con buffer circular de N muestras por `pg_proc_id_t`.
   Ver `docs/ROADMAP.md` §"Slice 2" — incluye extensión de M1 para `vmrss`
   (parser de `/proc/[pid]/statm`), tasas I/O (`/proc/[pid]/io`) y el
   período de gracia G=10 ciclos para procesos desaparecidos.
2. Deuda técnica de Slice 1 a retomar en Slice 3 (cuando haya 2+ módulos con
   tests): migrar a `build/tests/` con objetos ASAN reutilizables y
   `mkdtemp` para fixtures de procfs sintético (hoy `/tmp/pg_test_proc/`
   es fijo, colisiona con `-j`).
