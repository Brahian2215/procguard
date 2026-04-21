# Estado del Proyecto

## Estado actual

Slice 2 cerrado. `make asan && make test && make lint-funclen && make valgrind`
verdes. 23 tests totales (8 collector + 5 metrics + 10 store).

Módulos:
- **M1 Collector** (`src/collector/`): scan de procfs con `vmrss_bytes` (statm),
  counters I/O (io), filtro opcional de kernel threads.
- **M2 Sample Store** (`src/store/`): `init/insert/get_history/tick/destroy`
  con buffer circular por `pg_proc_id_t` y período de gracia G=10 ciclos
  (ADR-007). `pg_store_tick` libera entries vencidas vía swap-con-último.
- **M3 Metrics** (`src/metrics/`): `pg_metrics_cpu_percent` función pura con
  clamp `[0, 100*ncpus]` y sentinel `-1.0f` para casos inválidos.
- **main.c** (integrador Slice 1/2, temporal): dos scans con `sleep(1)`,
  store cableado (init → insert scan#1 → tick → insert scan#2 → destroy),
  top-5 por CPU% a stdout.

## Roadmap restante

| Slice | Objetivo | Estado |
|---|---|---|
| 2 | Extender M1 (statm, io, skip_kt) + M2 store + tick+gracia | ✅ cerrado |
| 3 | Completar M3 Metrics (tasas I/O; red diferida a Slice 4 o debt) | **Siguiente** |
| 4 | M4 Alert & Governance (políticas estáticas, histéresis, cooldown, dry-run) | Pendiente |
| 5 | Threading: hilos gobernanza + inotify, M5 Security (4 heurísticas) | Pendiente |
| 6 | M6 TUI (ncurses, tercer hilo) | Pendiente |
| 7 | M7 Report (JSON lines, snapshots, HTML) + acciones M4 reales | Pendiente |
| 8 | Modo daemon, hardening, SIGHUP reload | Pendiente |

Orden fijado por dependencias de datos: M1 → M2 → M3 → M4 → (M5‖M6) → M7.
Cada slice cierra con `make asan && make test && make valgrind` verdes.

## Próximos pasos (Slice 3)

Completar M3 Metrics Engine. Estado actual: sólo `pg_metrics_cpu_percent`
implementado. Faltan:

- **Tasas I/O** (4 funciones puras, una por counter): `rchar/s`, `wchar/s`,
  `read_bytes/s`, `write_bytes/s`. Counters ya recolectados por M1 en
  `pg_raw_sample_t`; sólo falta la aritmética `(curr - prev) / elapsed` con
  el mismo patrón de CPU% (sentinel `-1.0f`, validación de `(pid, starttime)`,
  elapsed vía `timestamp_ms`). Considerar una sola función
  `pg_metrics_io_rates(prev, curr, out)` que llene un struct con las 4
  tasas — reduce boilerplate y hay un único chequeo de sentinel.
- **RSS**: decisión de diseño — ¿M3 lo expone o se lee directo del sample?
  Es pase-directo, así que probablemente no necesita función propia; dejar
  nota en el plan.
- **Red**: recolección por proceso requiere `/proc/[pid]/net/dev` en el
  namespace del proceso (complicado con namespaces no propios). Evaluar
  si se difiere a Slice 4 junto con M5, o se registra como deuda técnica.

Crear `docs/plans/slice-3.md` al arrancar con brainstorming previo.

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
