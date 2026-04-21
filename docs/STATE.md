# Estado del Proyecto

## Estado actual

Slice 3 cerrado. `make asan && make test && make lint-funclen && make valgrind`
verdes. 28 tests totales (8 collector + 10 metrics + 10 store).

Módulos:
- **M1 Collector** (`src/collector/`): scan de procfs con `vmrss_bytes` (statm),
  counters I/O (io), filtro opcional de kernel threads.
- **M2 Sample Store** (`src/store/`): `init/insert/get_history/tick/destroy`
  con buffer circular por `pg_proc_id_t` y período de gracia G=10 ciclos
  (ADR-007). `pg_store_tick` libera entries vencidas vía swap-con-último.
- **M3 Metrics** (`src/metrics/`): completo para métricas derivables.
  `pg_metrics_cpu_percent` (función pura, clamp `[0, 100*ncpus]`, sentinel
  `-1.0f`, ADR-006) y `pg_metrics_io_rates` (llena `pg_io_rates_t` con
  rchar/wchar/read_bytes/write_bytes por segundo; sentinel por-counter ante
  underflow aislado; sin clamp superior). RSS es pase directo de
  `vmrss_bytes` sin función dedicada. Inyección explícita de hz/ncpus
  (ADR-008).
- **main.c** (integrador Slice 1/2, temporal): dos scans con `sleep(1)`,
  store cableado (init → insert scan#1 → tick → insert scan#2 → destroy),
  top-5 por CPU% a stdout. No consume aún tasas I/O — el integrador
  visible lo hará M4.

## Roadmap restante

| Slice | Objetivo | Estado |
|---|---|---|
| 2 | Extender M1 (statm, io, skip_kt) + M2 store + tick+gracia | ✅ cerrado |
| 3 | Completar M3 Metrics (tasas I/O; red diferida como deuda) | ✅ cerrado |
| 4 | M4 Alert & Governance (políticas estáticas, histéresis, cooldown, dry-run) | **Siguiente** |
| 5 | Threading: hilos gobernanza + inotify, M5 Security (4 heurísticas) | Pendiente |
| 6 | M6 TUI (ncurses, tercer hilo) | Pendiente |
| 7 | M7 Report (JSON lines, snapshots, HTML) + acciones M4 reales | Pendiente |
| 8 | Modo daemon, hardening, SIGHUP reload | Pendiente |

Orden fijado por dependencias de datos: M1 → M2 → M3 → M4 → (M5‖M6) → M7.
Cada slice cierra con `make asan && make test && make valgrind` verdes.

## Próximos pasos (Slice 4)

M4 Alert & Governance: parser inih de políticas estáticas (umbrales por
métrica, con histéresis y cooldown), evaluación por proceso en el loop de
gobernanza, revalidación de `(pid, starttime)` antes de actuar (ADR-005),
modo dry-run (acción = log). M3 ya expone CPU% y tasas I/O como insumos;
RSS se lee directo de `vmrss_bytes`.

Antes de arrancar, leer `docs/plans/slice-4-concurrency.md` (modelo de
tres hilos) — Slice 4 no introduce threading todavía, pero sus APIs deben
ser thread-safe por diseño para el cableado del Slice 5.

Crear `docs/plans/slice-4.md` con brainstorming previo.

## Deuda técnica

- **Red por-proceso diferida.** `/proc/[pid]/net/dev` fuera del netns del
  proceso no da datos por-PID — el archivo refleja el netns del lector
  (host procfs), no el del target. Para leer el netns del proceso hay
  que `setns` + reabrir el file, lo cual requiere `CAP_SYS_ADMIN`. Rompe
  la premisa "M3 = funciones puras" (exige I/O a procfs del target).
  Decisión de dónde vive la lectura pospuesta hasta que M5 defina acceso
  a procfs y/o se resuelva el modelo de privilegios.
- **Referencias ADR en M1 colgadas.** `src/collector/collector.{c,h}` cita
  ADR-021 (skip kernel threads) que no existe en `DECISIONS.md`. Limpiar
  en chore futuro: o se añade ADR-021 real, o se sustituye por texto sin
  número. Fuera del scope de Slice 3 (se limpió sólo lo relativo a M3).
- Patrón "compilar fuentes inline en cada test binary" no escala. Con
  Slice 4 (M4 añadirá `test_alert`) el dolor crece. Introducir
  `build/tests/` con objetos ASAN reutilizables.
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
