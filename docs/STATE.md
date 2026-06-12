# Estado del Proyecto

## Estado actual

**Slice 4c COMPLETO** (`cage` real, cgroups v2). `make asan && make test &&
make lint-funclen` verdes + `make valgrind` limpio. **111 tests**: 12 collector
+ 10 metrics + 10 store + 6 queues + 7 alert_parser + 8 alert_state + 58
alert_engine (8 init/config + 5 metric_current + 12 state machine + 10 validate
+ 11 act + 5 cage act + 5 cycle/gc/cage-gc/global). Target `make tsan`
(scaffold; Slice 5, ADR-015). Backend real de cage (`alert_cage.c`)
compilado/inspeccionado; ruta sysfs validable en máquina con privilegios.
**Siguiente: Slice 5 (threading+M5+red)** — ver Roadmap.

El binario `./build/procguard [--config <ini>] [--proc <base>]` corre 10 ciclos
`scan→insert→engine_cycle→store_tick→engine_gc`, top-5 CPU% a stdout y
decisiones del engine a stderr (`[alert] ... state=executed|dry_run|skip:<r>`).

Módulos:
- **M1 Collector** (`src/collector/`): scan de procfs con `vmrss_bytes` (statm),
  counters I/O (io), `exe_path` (readlink, ADR-016), filtro opcional de kernel
  threads. Helper `pg_collector_read_starttime` (guard TOCTOU de act).
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
  fail-loud a stderr.
- **M4 Alert state** (`src/alert/alert_state.c`, Slice 4b F3): registry
  `(id, policy_index) → pg_alert_state_t` con `upsert`/`lookup`/`gc`.
  Punteros estables (array de punteros heap); `gc` libera entries cuyo id ya
  no está en el store, alineado con `store_tick` (ADR-013).
- **M4 Alert engine** (`src/alert/alert.c` + `alert_eval.c`, Slice 4b F4):
  - `alert.h` API pública opaca + `pg_syscalls_t` (ADR-009).
  - `alert_internal.h` (NO público): struct engine completo +
    `pg_alert_decision_t` + prototipos de pasadas. Seam de testabilidad.
  - `pg_alert_engine_init/destroy`: carga catálogo (propaga código del
    loader: IO/PARSE/MEM) + registry + syscalls (NULL→libc; adaptador
    `libc_setpriority` por el enum interno de glibc).
  - `pg_alert_metric_current`: dispatch métrica→valor (cpu/io delta con
    sentinel; mem_rss instantáneo sin prev).
  - `pg_alert_evaluate`: state machine §5.4 con 3 zonas explícitas
    (above/banda-muerta/below), persistencia/histéresis, cooldown gate,
    escalamiento. Loop optimizado (history 1× por sample). Emite
    `pg_alert_decision_t[]` efímera (ADR-010).
  - `pg_alert_validate` (`alert_validate.c`, Fase 5b): pasada 2 **pura**.
    Anota `skip_reason` en orden: `stale_id` (sample ausente o starttime ≠,
    ADR-005), `protected` (whitelist ADR-012 con `exe_path`: pid==1/own_pid/
    ppid==own_pid/kernel-thread/protegido-nombre+ruta), `sanity` (cordura 5s
    §7 para STOP/KILL vía lookup de state).
  - `pg_alert_act` (`alert_act.c`, Fase 6): pasada 3. Log-primero (una línea
    por decisión, `state=executed|dry_run|skip:<reason>`). Solo
    `skip_reason==NULL`: guard TOCTOU (ADR-016, re-read `proc_base`/<pid> antes
    de RENICE/STOP/KILL; mismatch→`skip:gone`), dispatch por syscalls
    inyectados (ADR-009), techo kills/min (ring circular cap
    `max_kills_per_minute`; KILL sobre el techo→`skip:ceiling` transitorio, no
    avanza), `CAGE` real (Slice 4c, ADR-018: backend inyectable; TOCTOU propio +
    techo `max_caged_processes` no-stall + idempotencia; NULL/fallo→no-stall),
    no-stall (AFFINITY/TERM avanzan sin syscall, ADR-014), dry-run (avanza sin
    ejecutar ni TOCTOU). Tras ejecutar/dry-run: cooldown + reset persistence +
    avance vs reactivación. Modelo en ADR-017.
  - `pg_alert_engine_cycle`/`gc` (`alert.c`): ensamblan el pipeline efímero
    evaluate→validate→act (ADR-010) y el gc del registry + el gc de cages tras
    `store_tick` (ADR-013). `init` recibe `proc_base` (copia propia) y aloja el
    ring de kills + el registro de cages (cap `max_caged_processes`).
  - `pg_cage_apply_sysfs`/`pg_cage_release_sysfs` (`alert_cage.c`, Slice 4c):
    backend real cgroups v2 bajo `/sys/fs/cgroup/procguard/<pid>/` (mkdir +
    `+cpu` en subtree_control + `cpu.max` + `cgroup.procs`; release = rmdir).
    Best-effort → `PG_ERR_IO` ante fallo (act no-stall). Punteros por defecto de
    `pg_syscalls_t` (ADR-018); inyectable para tests sin root.
- **main.c** (orquestador de gobernanza, Slice 4b F7): args `--config`/`--proc`;
  carga `config/procguard.ini` (vía engine), loop 10 ciclos
  `scan→insert→engine_cycle→store_tick→engine_gc` (ADR-013), top-5 CPU% a
  stdout, decisiones a stderr. Secuencial (sin hilos: Slice 5). `now_ms` por
  ciclo con CLOCK_MONOTONIC; `sleep_ms` con `nanosleep`.

## Roadmap

| Slice | Objetivo | Estado |
|---|---|---|
| 2 | Extender M1 + M2 store + tick/gracia | ✅ |
| 3 | Completar M3 (tasas I/O; red diferida) | ✅ |
| 4a | M4 IPC queues + parser de políticas | ✅ |
| 4b-F3 | M4 `alert_state` registry | ✅ |
| 4b-F4 | M4 `evaluate` state machine + `metric_current` dispatch | ✅ |
| 4b-F5 | M1 `exe_path`/`read_starttime` (ADR-016) + M4 `validate` | ✅ |
| 4b-F6 | M4 `act` (no-stall ADR-014, techo kills, TOCTOU, ADR-017) + `cycle`/`gc` | ✅ |
| 4b-F7 | Integración `main.c` (orquestador real + INI bundled) | ✅ |
| 4c | `cage` real (cgroups v2 subárbol propio, backend inyectable ADR-018) | ✅ |
| 5 | Threading real (hilos gobernanza+inotify, validado bajo `make tsan` ADR-015) + M5 Security + red per-proceso | **Siguiente** |
| 6 | M6 TUI (ncurses, tercer hilo) | Pendiente |
| 7 | M7 Report (JSON lines, snapshots, HTML) | Pendiente |
| 8 | Modo daemon, SIGHUP reload, experimento (ver `plans/experiment-design.md`) | Pendiente |

Orden fijado por dependencias: M1 → M2 → M3 → M4 → (M5‖M6) → M7.
`cage` real (4c) puede intercalarse cuando se tenga la máquina con
privilegios/delegación; mientras tanto el no-stall (ADR-014) mantiene el
escalamiento funcional sin él.

## Próximos pasos (Slices 4b y 4c cerrados)

M4 Alert & Governance completo y secuencial, end-to-end en el binario, con
`cage` real (backend inyectable). El siguiente es **Slice 5**:

- **Slice 5 — Threading real + M5 Security + red per-proceso**: instanciar los
  3 hilos (gobernanza/inotify/TUI), validados bajo `make tsan` (ADR-015);
  activar políticas `type=security` (hoy `eval` las salta); leer
  `/proc/net/tcp` + `/proc/[pid]/fd` para `net_*`. **Leer
  `docs/plans/slice-4-concurrency.md` antes.**

Validación pendiente de la ruta real de cgroups del `cage` (smoke en máquina
con privilegios — ver deuda técnica). El experimento (Slice 8) ya tiene diseño
en `plans/experiment-design.md`.

El experimento (Slice 8) ya tiene diseño cerrado en
`docs/plans/experiment-design.md` (factorial 2×2×2, ANOVA con interacciones,
ground-truth externo, tasa de atribución separada).

## Deuda técnica

| Ítem | Motivo / nota | Destino |
|---|---|---|
| Red por-proceso (PDF §4.2: `/proc/net/tcp` + `/proc/[pid]/fd` readlink) | Alimenta `net_*` y heurística M5 port_scan. `net_sample_divisor=4` | Slice 5 con M5 |
| Métricas PDF §5.3 pendientes: `mem_vsize`, `thread_count`, `fd_count`, `net_connections`, `net_bytes_rate`, `net_sockets` | Catálogo canónico (10); M3 cubre 3 | Varios (4b/5/6/7) |
| Nota: `rchar/wchar_per_s` no son del catálogo PDF; M4 mapea `io_read_rate→read_bytes_per_s`, `io_write_rate→write_bytes_per_s` | Valor añadido interno | — |
| Diferenciación de `errno` en M1 (ENOENT/ESRCH/EACCES silenciosos vs ENOMEM/EIO en log, PDF §6 Nivel 1) | Requiere canal de log; llega con M7 | Slice 7 o antes si M4 lo exige |
| `sample_buffer=16` en `config/procguard.ini` (PDF default 120) | Valor de demo/dev; main ya lo lee del `[global]` (resuelto el hardcode). Subir a 120 para producción | Ajuste de config, no de código |
| `vmrss_bytes` desde statm (PDF prefiere `/status`) | `status` unifica `VmSize` + UID real/efectivo | Cuando entren `mem_vsize` o disguised_process (M5) |
| Path fijo `/tmp/pg_test_proc` para fixtures | Migrar a `mkdtemp` cuando crezca el número de binarios con procfs | Cuando aparezca el tercero |
| `/proc/[pid]/io` requiere root o mismo UID | Como usuario normal, counters en 0. No es bug | — |
| Ring buffers duplicados en `queue.c` (inotify + command) | Tolerable a 2 tipos; refactor a macro si aparece un 3º | Cuando haya 3ª cola |
| Validación de la ruta real de cgroups del `cage` (mkdir + `cpu.max` + `cgroup.procs` + verificación del kernel) | El backend `alert_cage.c` está implementado/inspeccionado pero sin smoke real (requiere root/delegación; los unit-tests usan backend inyectado) | Máquina con privilegios |
| `memory.max` (segundo límite del `cage`, además de `cpu.max`) | 4c implementó solo `cpu.max` (lever del experimento); memoria es scope extra | Post-4c si se necesita |
| Modelo de privilegios del `cage` (capabilities acotadas / unit systemd `Delegate=yes`) documentado pero no codificado | ADR-014; el backend asume el subárbol escribible en runtime | Despliegue/Slice 8 |
| `threshold` de `mem_rss` en bytes crudos (parser guarda `float`, sin sufijos K/M/G; pierde precisión >16 MB) | Comparación de umbral tolera la imprecisión; usabilidad del INI mejora con parser de sufijos | Cuando el INI lo necesite (post-4b) |
| Tests con hilos bajo `make tsan` (árbol de objetos paralelo; TSan no mezcla con ASan) (ADR-015) | ASan no detecta races; hoy colas testeadas secuencialmente | Slice 5 |
| `make valgrind`/`make tsan` requieren build sin ASan (sanitizers no conviven) | Workflow `make clean` previo | Target `*-ci` cuando exista CI |

Sección 5.11 del PDF (concurrencia) resumida en
[plans/slice-4-concurrency.md](plans/slice-4-concurrency.md) — leer antes de
Slice 5. Diseño experimental rigorizado en
[plans/experiment-design.md](plans/experiment-design.md) — leer antes de
Slice 8. Planes cerrados (Slice 4b, 4c) en `plans/archive/`.
