# Estado del Proyecto

## Estado actual

Slice 1 cerrado + M1/M2 extendidos (statm, io, kernel thread filter, store
básico). `make asan && make test && make lint-funclen && make valgrind`
verdes. 20 tests totales (8 collector + 5 metrics + 7 store).

Módulos:
- **M1 Collector** (`src/collector/`): scan de procfs con `vmrss_bytes` (statm),
  counters I/O (io), filtro opcional de kernel threads.
- **M2 Sample Store** (`src/store/`): `init/insert/get_history/destroy` con
  buffer circular por `pg_proc_id_t`. Falta `pg_store_tick` + gracia G=10.
- **M3 Metrics** (`src/metrics/`): `pg_metrics_cpu_percent` función pura con
  clamp `[0, 100*ncpus]` y sentinel `-1.0f` para casos inválidos.
- **main.c** (integrador Slice 1/2, temporal): dos scans con `sleep(1)`,
  top-5 por CPU% a stdout.

## Roadmap restante

| Slice | Objetivo | Estado |
|---|---|---|
| 2 | Extender M1 (statm, io, skip_kt) + M2 store + tick+gracia | **En curso** — falta tick/gracia y cablear store en main |
| 3 | M4 Alert & Governance (políticas estáticas, histéresis, cooldown, dry-run) | Pendiente |
| 4 | Threading: hilos gobernanza + inotify, M5 Security (4 heurísticas) | Pendiente |
| 5 | M6 TUI (ncurses, tercer hilo) | Pendiente |
| 6 | M7 Report (JSON lines, snapshots, HTML) + acciones M4 reales | Pendiente |
| 7 | Modo daemon, hardening, SIGHUP reload | Pendiente |

Orden fijado por dependencias de datos: M1 → M2 → M3 → M4 → (M5‖M6) → M7.
Cada slice cierra con `make asan && make test && make valgrind` verdes.

## Próximos pasos (Slice 2, lo que queda)

1. `pg_store_tick(store, grace_cycles)` — incrementa `absent_cycles` y libera
   entries vencidas. Ver `docs/plans/slice-2.md`.
2. Cablear store en `main.c`: insertar scans, llamar tick entre medias.
3. Smoke de `make valgrind` con el store en el path caliente.

## Deuda técnica

- Patrón "compilar fuentes inline en cada test binary" no escala. En Slice 3+
  introducir `build/tests/` con objetos ASAN reutilizables.
- Path fijo `/tmp/pg_test_proc` para fixtures. Migrar a `mkdtemp` cuando
  haya más de un binario con fixtures de procfs.
- `make valgrind` requiere build sin ASAN (los dos sanitizers no conviven):
  `make clean && make debug && make valgrind`. Considerar target
  `valgrind-ci` que haga el reset internamente cuando exista CI.
- `/proc/[pid]/io` requiere root o mismo UID; corriendo como usuario
  normal la mayoría de procesos tendrán los 4 counters en 0. No es bug.
- Sección 5.11 del PDF (concurrencia detallada: tamaños exactos de colas,
  protocolo de mutex) leerse antes de Slice 4 — resumen en
  `docs/plans/slice-4-concurrency.md`.
