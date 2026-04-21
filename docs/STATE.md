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

- **Red por-proceso (correlación socket↔proceso).** PDF §4.2 prescribe:
  leer la tabla global de sockets desde `/proc/net/tcp` + `/proc/net/udp`,
  recorrer `/proc/[pid]/fd/` con `readlink()` identificando descriptores
  socket (formato `socket:[inodo]`), y correlacionar inodos para mapear
  conexiones ↔ PID. No requiere privilegios especiales (a diferencia de
  leer `/proc/[pid]/net/dev` en un netns ajeno). Alimenta tres métricas
  (`net_connections`, `net_bytes_rate`, `net_sockets`) y la heurística
  M5 port_scan. Por costo computacional, PDF §4.2 define
  `net_sample_divisor = 4` (correlación cada N ciclos, no cada ciclo).
  Destino: **Slice 5** junto con M5 Security (consumidor natural).

- **Métricas del catálogo PDF §5.3 pendientes.** ProcGuard declara 10
  métricas canónicas; M3 actual cubre 3. Pendientes con fuente y slice
  objetivo:

  | Métrica | Identificador | Fuente | Slice |
  |---|---|---|---|
  | Memoria virtual total | `mem_vsize` | `/proc/[pid]/status` VmSize | Slice 4 si una política lo requiere; si no, Slice 7 (snapshots forenses) |
  | Número de hilos | `thread_count` | `/proc/[pid]/stat` campo 20 (`num_threads`) | Slice 6 (TUI) |
  | File descriptors abiertos | `fd_count` | contar entradas en `/proc/[pid]/fd/` | Slice 7 (snapshots) |
  | Conexiones de red | `net_connections` | correlación inodos (ver entrada anterior) | Slice 5 |
  | Tasa de bytes de red | `net_bytes_rate` | correlación + `/proc/net/tcp` counters | Slice 5 |
  | Sockets abiertos | `net_sockets` | filtrar `/proc/[pid]/fd/` por prefijo `socket:` | Slice 5 |

  Nota: `rchar/wchar_per_s` **no** son identificadores del catálogo PDF.
  Se exponen como valor añadido, pero M4 sólo mapea `io_read_rate` →
  `read_bytes_per_s` e `io_write_rate` → `write_bytes_per_s` (bytes reales
  a disco, no bytes lógicos con cache hits).

- **Diferenciación de `errno` en M1 (PDF §6 Nivel 1).** La spec exige que
  fallos `ENOENT`/`ESRCH`/`EACCES` se descarten silenciosamente (normales:
  proceso terminado durante scan, permisos) y que `ENOMEM`/`EIO` se
  registren en log. Actualmente `read_file()` en
  [src/collector/collector.c:49](src/collector/collector.c#L49)
  retorna `PG_ERR_IO` sin tocar `errno`; todos los errores son silenciados
  por igual. Implementar requiere `#include <errno.h>`, propagación de
  `errno` en el stack de helpers, y un canal de log (hoy no existe;
  llegará con M7). Destino: **Slice 7** junto con M7 Report o antes si
  M4 necesita observabilidad de fallos.

- **`sample_buffer` hardcodeado a N=16** en el integrador. PDF §5.4 define
  default 120. Alinear cuando el parser de config (M4) lea `[global]`.

- **`vmrss_bytes` se lee de `/proc/[pid]/statm` (campo 2 × pagesize).**
  PDF §4.1 prefiere `/proc/[pid]/status` (campo `VmRSS`). Equivalentes
  semánticamente, pero `status` unifica la lectura de `VmSize` y UID
  real/efectivo (necesario para M5 disguised_process). Migrar cuando
  `mem_vsize` o la heurística de UID entren en scope.

- Patrón "compilar fuentes inline en cada test binary" no escala. Con
  Slice 4 (M4 añadirá `test_alert`) el dolor crece. Introducir
  `build/tests/` con objetos ASAN reutilizables **al inicio del Slice 4**.

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
