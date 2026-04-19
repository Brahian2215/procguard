# Estado del Proyecto

## Última actualización
Sesión 2 — 2026-04-19 — M1 Collector funcional con tests verdes y Makefile completo

## Módulos completados
- M1 Data Collector (parcial): `pg_collector_init/scan/destroy` con procfs path
  configurable. Parser de `/proc/[pid]/stat` robusto a `comm` con paréntesis
  internos. ASAN limpio (0 leaks). Pendiente para slices futuros: `vmrss`
  (Slice 2), gracia G=10 ciclos para procesos desaparecidos (Slice 2+),
  filtrado de kernel threads (Slice 2 con TUI).

## Tests pasando
- `tests/unit/test_collector.c` — 5 tests, 5 passed, 0 failed
  - `test_scan_finds_all_three`
  - `test_disappeared_process`
  - `test_recycled_pid`
  - `test_malformed_stat_is_skipped`
  - `test_parses_comm_with_internal_parens`

## Estado del build
- `make asan` — exit 0, cero warnings bajo `-Wall -Wextra -Werror -Wshadow
  -Wpointer-arith -Wcast-align -Wstrict-prototypes -Wmissing-prototypes`.
- `make test` — verde.
- `make valgrind` — saldrá con exit 1 hasta Sesión 3 (el stub `src/main.c`
  retorna 1; valgrind no detecta leaks). Documentado en TODO y DECISIONS.

## Última acción ejecutada
add slice-1 session-1: collector with synthetic procfs tests (commit pendiente)

## Próximos pasos
1. Slice 1 / Sesión 2: M3 Metrics — `pg_metrics_cpu_percent(prev, curr, hz)`
   con clamp a `[0, 100*ncpus]`, validación de `pg_proc_id_t` (revalida pid+
   starttime entre muestras). Tests en `tests/unit/test_metrics.c` según
   `docs/plans/slice-1.md` §6–§8.
2. Slice 1 / Sesión 3: `src/main.c` real (dos scans con `sleep(1)`, qsort
   por CPU%, top-5 a stdout). Hace verde el target `make valgrind`.
