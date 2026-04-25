# Estado del Proyecto

## Estado actual

Slice 4a cerrado (Fases 1 y 2). `make asan && make test && make lint-funclen &&
make valgrind` verdes. **41 tests**: 8 collector + 10 metrics + 10 store +
6 queues + 7 alert_parser.

Módulos:
- **M1 Collector** (`src/collector/`): scan de procfs con `vmrss_bytes` (statm),
  counters I/O (io), filtro opcional de kernel threads.
- **M2 Sample Store** (`src/store/`): `init/insert/get_history/tick/destroy`
  con buffer circular por `pg_proc_id_t` y gracia G=10 (ADR-007).
- **M3 Metrics** (`src/metrics/`): `cpu_percent` (ADR-006/008) e `io_rates`
  (sentinel por-counter ante underflow aislado). RSS es pase directo de
  `vmrss_bytes`.
- **M4 IPC** (`src/ipc/queue.c`, Slice 4a F1): `pg_results_t`,
  `pg_inotify_event_queue_t`, `pg_command_queue_t` — ring buffers
  mutex-protegidos con drop-oldest. Slice 4 no instancia hilos; la infra
  queda lista para cablear Slice 5/6.
- **M4 Policy parser** (`src/alert/alert_policy.c`, Slice 4a F2): carga INI
  (inih) a catálogo tipado `pg_policy_t[]` + `pg_global_config_t` +
  `pg_security_config_t`. Validación multi-error con acumulador, reporte
  fail-loud a stderr. ADRs 009-013 cubren state machine y syscalls
  (pendiente implementación).
- **main.c** (integrador temporal Slice 1/2): dos scans con `sleep(1)`,
  store cableado, top-5 por CPU%. No consume tasas I/O ni carga INI — el
  integrador real lo hará Slice 4b.

## Roadmap

| Slice | Objetivo | Estado |
|---|---|---|
| 2 | Extender M1 + M2 store + tick/gracia | ✅ |
| 3 | Completar M3 (tasas I/O; red diferida) | ✅ |
| 4a | M4 IPC queues + parser de políticas | ✅ |
| 4b | M4 state machine (evaluate/validate/act), engine_gc, integración en main | **Siguiente** |
| 5 | Threading real (hilos gobernanza+inotify) + M5 Security | Pendiente |
| 6 | M6 TUI (ncurses, tercer hilo) | Pendiente |
| 7 | M7 Report (JSON lines, snapshots, HTML) + acciones M4 reales | Pendiente |
| 8 | Modo daemon, hardening, SIGHUP reload | Pendiente |

Orden fijado por dependencias: M1 → M2 → M3 → M4 → (M5‖M6) → M7.

## Próximos pasos (Slice 4b)

Implementar la state machine de alerta con los ADRs 009-013 ya registrados:
- `pg_alert_state_t` por política×id con `persistence`/`hysteresis`/`cooldown`.
- Pipeline `evaluate → validate → act` con lista efímera (ADR-010).
- Whitelist dinámica `ppid == own_pid` en runtime (ADR-012).
- `engine_gc` alineado con `store_tick` (ADR-013).
- Syscalls (kill/setpriority) inyectadas vía struct para test (ADR-009).
- Cablear main.c como orquestador: cargar INI, loop de N iteraciones,
  aplicar `sample_buffer` del `[global]`.

## Deuda técnica

| Ítem | Motivo / nota | Destino |
|---|---|---|
| Red por-proceso (PDF §4.2: `/proc/net/tcp` + `/proc/[pid]/fd` readlink) | Alimenta `net_*` y heurística M5 port_scan. `net_sample_divisor=4` | Slice 5 con M5 |
| Métricas PDF §5.3 pendientes: `mem_vsize`, `thread_count`, `fd_count`, `net_connections`, `net_bytes_rate`, `net_sockets` | Catálogo canónico (10); M3 cubre 3 | Varios (4b/5/6/7) |
| Nota: `rchar/wchar_per_s` no son del catálogo PDF; M4 mapea `io_read_rate→read_bytes_per_s`, `io_write_rate→write_bytes_per_s` | Valor añadido interno | — |
| Diferenciación de `errno` en M1 (ENOENT/ESRCH/EACCES silenciosos vs ENOMEM/EIO en log, PDF §6 Nivel 1) | Requiere canal de log; llega con M7 | Slice 7 o antes si M4 lo exige |
| `sample_buffer=16` hardcoded en main.c (PDF default 120) | Alinear cuando main cargue `[global]` | Slice 4b |
| `vmrss_bytes` desde statm (PDF prefiere `/status`) | `status` unifica `VmSize` + UID real/efectivo | Cuando entren `mem_vsize` o disguised_process (M5) |
| Path fijo `/tmp/pg_test_proc` para fixtures | Migrar a `mkdtemp` cuando crezca el número de binarios con procfs | Cuando aparezca el tercero |
| `make valgrind` requiere build sin ASAN (reset interno) | Los dos sanitizers no conviven | Target `valgrind-ci` cuando exista CI |
| `/proc/[pid]/io` requiere root o mismo UID | Como usuario normal, counters en 0. No es bug | — |
| Ring buffers duplicados en `queue.c` (inotify + command) | Tolerable a 2 tipos; refactor a macro si aparece un 3º | Cuando haya 3ª cola |

Sección 5.11 del PDF (concurrencia) resumida en
[plans/slice-4-concurrency.md](plans/slice-4-concurrency.md) — leer antes de
Slice 5. Planes cerrados (slice-2, slice-3) en `plans/archive/`.
