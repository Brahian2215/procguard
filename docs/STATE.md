# Estado del Proyecto

## Última actualización
Sesión 2 — 2026-04-19 — M3 Metrics CPU% implementado con 8 tests verdes

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

## Estado del build
- `make asan` — exit 0, cero warnings bajo `-Wall -Wextra -Werror -Wshadow
  -Wpointer-arith -Wcast-align -Wstrict-prototypes -Wmissing-prototypes`.
- `make test` — verde (13 tests totales: 5 collector + 8 metrics).
- `make lint-funclen` — OK (todas las funciones <= 50 líneas).
- `make valgrind` — saldrá con exit 1 hasta Sesión 3 (el stub `src/main.c`
  retorna 1; valgrind no detecta leaks). Documentado en TODO y DECISIONS.

## Última acción ejecutada
add slice-1 session-2: metrics cpu-percent with tests (5dfcd6e)

## Próximos pasos
1. Slice 1 / Sesión 3: `src/main.c` real — dos scans de M1 con `sleep(1)`,
   pareo por `pg_proc_id_t`, cálculo de CPU% con `pg_metrics_cpu_percent`,
   `qsort` descendente y top-5 a stdout. Enlazar `metrics.c` en
   `$(BUILD_DIR)/procguard`. Hace verde `make valgrind` y cierra Slice 1
   según `docs/plans/slice-1.md` §9–§10.
2. Slice 2: M2 Sample Store + extensión de M1 (vmrss, I/O, gracia G=10).
