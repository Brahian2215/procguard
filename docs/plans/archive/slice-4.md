# Slice 4b — M4 Alert & Governance (state machine + integración)

Fases 0-2 cerradas: housekeeping (ADRs 009-013, Makefile `build/tests/*.o`,
vendor inih), IPC queues (`src/ipc/queue.c`, 6 tests), parser de políticas
(`src/alert/alert_policy.c`, 7 tests). Quedan las Fases 3-7. Un commit
verde por fase. Target al cierre: **~70 tests**.

## Árbol pendiente

```
src/alert/
├── alert.{h,c}          # engine init/cycle/gc/destroy
├── alert_state.{h,c}    # registry (upsert/lookup/gc)
├── alert_eval.c         # evaluate_policy(sample, policy, state, store)
├── alert_validate.c     # whitelist, techos, cordura, reválida starttime
└── alert_act.c          # warn/renice/stop/kill + dry-run

tests/unit/
├── test_alert_state.c     (≥4)
├── test_alert_eval.c      (≥10)
├── test_alert_validate.c  (≥9)
└── test_alert_act.c       (≥8)
```

## Contratos nuevos

```c
typedef struct {
    pg_proc_id_t        id;
    size_t              policy_index;
    unsigned int        persistence;
    unsigned int        hysteresis;
    unsigned long long  cooldown_until_ms;
    unsigned long long  alert_active_since_ms;      /* §7 cordura 5s */
    int                 escalation_level;
    bool                deactivated_since_last_act; /* avance vs reactivación */
} pg_alert_state_t;

typedef struct {
    int (*kill)       (pid_t, int);
    int (*setpriority)(int, id_t, int);
} pg_syscalls_t;   /* ADR-009 — libc por default, stubs en tests */

int  pg_alert_engine_init(pg_alert_engine_t **eng, const char *ini_path,
                          pid_t own_pid, long hz, long ncpus);
int  pg_alert_engine_cycle(pg_alert_engine_t *eng,
                           const pg_raw_sample_t *samples, size_t n,
                           const pg_store_t *store, unsigned long long now_ms);
void pg_alert_engine_gc(pg_alert_engine_t *eng, const pg_store_t *store);
void pg_alert_engine_destroy(pg_alert_engine_t *eng);
```

Orquestación: `cycle → store_tick → engine_gc` (ADR-013).

## Fases

### Fase 3 — Registry `alert_state` (`feat(alert):`)

Array plano de `(id, policy_index) → state`. `upsert` devuelve puntero;
`gc` libera entries cuyo id ya no está en el store (post `pg_store_tick`).
Tests: `upsert_creates_and_preserves`, `starttime_change_creates_new_entry`
(ADR-005), `gc_releases_after_m2_expired`, `gc_keeps_present_in_store`.

### Fase 4 — `evaluate` (`feat(alert):`)

#### Seam de testabilidad (decisión estructural) — `alert_internal.h`

Las tres pasadas viven en `.c` separados pero comparten el struct del engine
y el tipo decisión. `src/alert/alert_internal.h` (NO público) define:
`pg_alert_engine_t` completo + `pg_alert_decision_t` + prototipos de las
pasadas. `alert.h` mantiene el engine **opaco** + API pública. Los `.c` de
alert y los tests incluyen `alert_internal.h`.

```c
typedef struct {
    pg_proc_id_t     id;
    size_t           policy_index;
    pg_action_kind_t kind;          /* acción del nivel emitido */
    int              param;
    float            metric_value;  /* valor que disparó (para log) */
    float            threshold;
    int              level;         /* escalation_level emitido */
    const char      *skip_reason;   /* NULL en evaluate; set en validate (literal, sin ownership) */
} pg_alert_decision_t;

struct pg_alert_engine {
    pg_policy_t              *policies;   size_t n_policies;
    pg_global_config_t        global;
    pg_security_config_t      security;
    pg_alert_state_registry_t *states;
    pg_syscalls_t             sc;         /* ADR-009: libc por default */
    pid_t                     own_pid;
    long                      hz, ncpus;
    /* Fase 6 añade (struct append-only): ring de timestamps para el techo
     * kills/min (tamaño = global.max_kills_per_minute, alloc en init) y
     * contador de caged para max_caged_processes. No requerido en Fase 4. */
};

/* out dimensionado por el caller a n_samples * n_policies. Retorna n_dec. */
size_t pg_alert_evaluate(pg_alert_engine_t *e, const pg_raw_sample_t *s,
                         size_t n, const pg_store_t *st,
                         unsigned long long now_ms, pg_alert_decision_t *out);
```

`pg_alert_engine_init` (corrige ADR-009 — el plan original omitía `sc`):

```c
int pg_alert_engine_init(pg_alert_engine_t **eng, const char *ini_path,
                         pid_t own_pid, long hz, long ncpus,
                         const pg_syscalls_t *sc); /* NULL → libc */
```

#### Dispatch de métrica (helper propio, `static` + tests vía evaluate)

`metric_current(metric_id, prev, curr, hz, ncpus) -> float`:

- `PG_METRIC_CPU_PERCENT` → `pg_metrics_cpu_percent(prev,curr,hz,ncpus)`
  (sentinel `-1.0f` si `prev==NULL` o id mismatch o underflow).
- `PG_METRIC_IO_READ_RATE` → `pg_metrics_io_rates(prev,curr,&r)`;
  retorna `r.read_bytes_per_s` (puede ser `-1.0f` por-counter).
- `PG_METRIC_IO_WRITE_RATE` → análogo con `r.write_bytes_per_s`.
- `PG_METRIC_MEM_RSS` → `(float)curr->vmrss_bytes`. **No usa `prev`, nunca
  retorna sentinel** (RSS es valor instantáneo válido siempre).

#### Loop (optimizado — samples afuera, history 1×)

Por cada `sample[i]`: `pg_store_get_history(st, id, hist, 2, &hlen)` **una
vez**; `curr=&s[i]`, `prev = hlen==2 ? &hist[0] : NULL`. Loop interno sobre
políticas reusa `prev`. Evita O(N²·P): O(N) lookups (cada O(N)) + O(N·P) eval.

#### State machine PDF §5.4 (con banda muerta explícita)

Por `(sample, policy)`, con `m = metric_current(...)`:

1. `policy.type==SECURITY` → skip (activa en Slice 5).
2. `m < 0.0f` (sentinel: sin `prev` utilizable) → **freeze** (no toca state).
3. **Tres zonas** (fronteras `==` caen en banda muerta):
   - `m > threshold` (**above**): `persistence++`, `hysteresis=0`.
     Si `persistence == policy.persistence` y `alert_active_since_ms==0` →
     `alert_active_since_ms = now`.
   - `threshold_low <= m <= threshold` (**banda muerta**): `persistence=0`,
     `hysteresis=0` (racha rota en ambos sentidos; ni escala ni desactiva).
   - `m < threshold_low` (**below**): `hysteresis++`, `persistence=0`.
4. **Desactivación**: si `hysteresis >= policy.hysteresis_m` y la alerta
   estaba activa → reset contadores, `alert_active_since_ms=0`,
   `deactivated_since_last_act=true`. **No** decrementa `escalation_level`.
5. **Emisión**: si `persistence >= policy.persistence` y
   `now >= cooldown_until_ms` y `escalation_level < n_actions` → emite
   decisión con `kind=actions[escalation_level]`.
6. `escalation_level >= n_actions` → log "exhausted", sin nueva decisión.

`act()` (Fase 6) aplica el avance: `deactivated_since_last_act==false` →
`level++`; `true` → mismo nivel (reactivación). En ambos:
`cooldown_until_ms = now + cooldown_s*1000`, `persistence=0`, flag a false.
El reset de `persistence` en act + el cooldown forman el doble gate del PDF.

Ausencia de sample en el ciclo → el `(id,policy)` simplemente no se visita →
freeze natural (ADR-013).

#### Tests (≥12, +2 sobre el plan original)

`above_threshold_persistence++`, `below_low_hysteresis++`,
**`dead_band_resets_both_counters`** (nuevo, hueco B),
**`mem_rss_uses_vmrss_no_prev`** (nuevo, hueco A),
`io_rate_metric_extracted`, `sentinel_freezes`, `absent_freezes`,
`persistence_reached_emits`, `persistence_1_immediate`, `cooldown_blocks`,
`cooldown_0_immediate`, `hysteresis_deactivates_preserving_level`,
`reactivation_same_level`, `post_cooldown_advances_level`,
`full_escalation_warn_renice_stop_kill`, `exhausted_no_new_decision`,
`multiple_policies_same_proc`, `security_skipped`.

### Fase 5a — M1 enriquece el sample con `exe_path` (`feat(collector):`) — ADR-016

`validate` necesita la ruta del ejecutable; por ADR-001 la lee M1, no M4.

- `pg_raw_sample_t.exe_path[PG_EXE_MAX]` (append-only; `PG_EXE_MAX` = 512,
  truncado documentado). `readlink(<proc_base>/<pid>/exe)`; fallo (ENOENT
  kernel thread / proceso muerto / EACCES) → `exe_path[0]='\0'`. readlink no
  null-termina: terminar manualmente.
- `pg_collector_read_starttime(const char *proc_base, pid_t pid,
  unsigned long long *out)` → re-lee campo 22 de `stat` para el guard TOCTOU
  de `act()` (ADR-016 parte 2). Retorna PG_OK / PG_ERR_IO (no existe) /
  PG_ERR_PARSE.
- Tests (≥4, fixture con `symlink()`): `exe_path_resolved`,
  `exe_empty_for_kernel_thread` (sin symlink exe), `exe_truncated_marked`,
  `read_starttime_matches` + `read_starttime_missing_pid_io_err`.

### Fase 5b — `validate` (`feat(alert):`)

Firma: `void pg_alert_validate(pg_alert_engine_t *eng,
const pg_raw_sample_t *samples, size_t n, pg_alert_decision_t *decs,
size_t n_dec, unsigned long long now_ms)`. **Pura** (sin I/O): opera sobre
los samples (incluido `exe_path`) + `eng->global`/`eng->security` + lookup de
state para `alert_active_since_ms`. Setea `skip_reason` (literal estático),
en orden, primer hit gana:

1. **Consistencia id**: el sample del ciclo con ese pid no existe o su
   starttime ≠ `dec.id.starttime` → `stale_id`. (El guard TOCTOU autoritativo
   está en `act()`, ADR-016 parte 2.)
2. **Whitelist** (ADR-012): `pid==1`, `pid==own_pid`, `ppid==own_pid`,
   kernel thread (`exe_path==""`), o (`comm∈protected_names` **∧** `exe_path`
   con prefijo en `protected_paths`) → `protected`. `protected_paths` viene de
   `[security]`; si vacío, default `{/usr/bin,/usr/sbin,/bin,/sbin}`.
3. **Cordura 5s** (PDF §7): `kind∈{STOP,KILL}` y `now -
   alert_active_since_ms < 5000` → `sanity` (lookup de state).

El **techo kills/min** NO está aquí: vive en `act()` (Fase 6) junto al ring
que él mismo escribe, manteniendo `validate` puro (sample+config+state, sin
estado de kills).

Tests (≥9): `valid_passes_skip_null`, `whitelist_pid_1`, `whitelist_own_pid`,
`whitelist_child_by_ppid`, `whitelist_protected_std_path`,
`whitelist_protected_nonstd_not_protected`, `whitelist_kernel_thread_empty_exe`,
`stale_starttime_skipped`, `sanity_5s_blocks_kill`, `sanity_ok_after_5s`.

### Fase 6 — `act` ✅ CERRADA (2026-06-11, TDD)

**101 tests verdes** (+16 en `test_alert_engine`: 2 init proc_base/ring + 7
dispatch/advance + 4 TOCTOU/dry-run/ceiling + 3 cycle/gc) bajo ASan+leak +
valgrind (binario) limpio + lint-funclen OK. Implementado en
`src/alert/alert_act.c` (`pg_alert_act`) + `cycle`/`gc` en `alert.c`. ADR-017
registrado (modelo de ejecución de act).

**Desviaciones respecto al plan original (resueltas con defaults anclados):**
- **Contador `caged` diferido a Slice 4c.** El plan pedía añadirlo al engine
  junto al ring, pero ninguna acción de 4b lo lee/escribe (cage es 4c por
  ADR-014) → sería campo muerto. Solo se añadió el ring de kills (KILL lo usa).
- **Tests de TOCTOU con procfs sintético** (no inyección): `read_starttime` es
  una lectura de procfs, no un syscall destructivo; se testea con fixtures
  `/tmp/pg_test_act_proc/<pid>/stat` (patrón de `test_collector`), linkeando
  `collector.o`. Consistente con los dos patrones del proyecto (procfs
  sintético para lecturas; inyección ADR-009 solo para kill/setpriority).
- **`proc_base` se guarda como copia propia** (malloc+memcpy) en el engine,
  liberada en `destroy`.
- **Decisiones de ejecución** (ADR-017): techo = skip transitorio `ceiling`
  (no avanza, reintenta); no-stall avanza; dry-run omite TOCTOU.

Resumen original de la fase (cumplido salvo lo anotado):

Antes de Fase 6: añadir a `struct pg_alert_engine` (append-only) `proc_base`,
el ring de kills (timestamps, tamaño `max_kills_per_minute`) y el contador de
caged; y `proc_base` a `pg_alert_engine_init`.

**Guard TOCTOU (ADR-016 parte 2):** para `kind∈{RENICE,STOP,KILL}`, justo
antes del syscall, `pg_collector_read_starttime(proc_base, pid, &st)`; si
`st != dec.id.starttime` o el pid desapareció → cancela, log "proceso
desaparecido antes de acción", **no** avanza nivel.

**Techo kills/min** (ADR-014, movido aquí desde Fase 5b): el ring de
timestamps vive en el engine; helper `kills_last_minute(eng, now)` cuenta los
de `[now-60000, now]`. Antes de un `KILL`: si `>= max_kills_per_minute` →
skip `ceiling` (modo solo-alertas). Tras un `KILL` ejecutado: push del
timestamp al ring. `max_caged_processes` espera a `cage`.

**Log primero** (todas las decisiones, incluyendo skips y dry-run).
Dispatch sólo para `skip_reason==NULL` y que pasen el guard TOCTOU:

- `WARN` → log.
- `RENICE` → `syscalls->setpriority(PRIO_PROCESS, pid, param)`.
- `STOP`  → `syscalls->kill(pid, SIGSTOP)`.
- `KILL`  → `syscalls->kill(pid, SIGKILL)` + push ring kills/min.
- `AFFINITY / CAGE / TERM` → log "not yet implemented" pero **SÍ avanza
  nivel y aplica cooldown** (ADR-014 no-stall): una acción indisponible se
  trata como ejecutada para el escalamiento, de modo que la secuencia
  siempre puede llegar a `stop`/`kill`. Nunca se atasca.

**Dry-run** (`cfg.dry_run==true`): loggea `[DRY-RUN] would <kind>` pero
**sí** avanza nivel/cooldown (para ver la secuencia antes de dar poder).

Tras ACT ejecutada o dry-run: `cooldown_until_ms = now + cooldown_s*1000`,
reset persistence; avance según `deactivated_since_last_act` (Fase 4).

Tests (≥8): `warn_no_syscall`, `renice_calls_setpriority`,
`stop_calls_kill_sigstop`, `kill_sigkill_and_pushes_ring`,
`cage_logs_not_impl_no_syscall`, `dry_run_suppresses_syscalls_advances_level`,
`skip_protected_is_logged`, `escalation_advance_vs_reactivation`.

### Fase 7 — Integración `main.c` ✅ CERRADA (2026-06-11) — **Slice 4b COMPLETO**

**103 tests verdes** (+2: accessor `pg_alert_engine_global`) ASan+leak +
`make valgrind` limpio sobre el orquestador real (5 s) + lint-funclen OK. El
binario `./build/procguard` carga `config/procguard.ini`, corre 10 ciclos
`scan→insert→engine_cycle→store_tick→engine_gc` (ADR-013) e imprime top-5 CPU%;
las decisiones del engine salen por stderr desde `act()`. Verificado en vivo:
con un umbral bajo de prueba la secuencia escala WARN→RENICE en dry-run y el
STOP queda bloqueado por la cordura 5s (`state=sanity`) — pipeline completo.

**Decisiones de la fase:**
- **Accessor `pg_alert_engine_global()`** (nuevo en `alert.h`): el engine es
  opaco pero el orquestador necesita `sample_interval`/`sample_buffer` del
  `[global]` sin re-parsear. Devuelve puntero de solo-lectura. Único código con
  lógica propia de la fase → con TDD; el resto de `main.c` es glue (CLAUDE.md
  §2: validado con valgrind + ejecución, sin tests unitarios dedicados).
- **Decisiones por stderr** (no stdout enriquecido): `cycle` queda encapsulado
  (ADR-010), `act()` ya loguea cada decisión. stdout = top-5 CPU%.
- **Makefile**: la regla del binario linkea M1/M2/M3 + engine M4 completo +
  inih en un solo `gcc` (inih compila limpio bajo flags estrictas, verificado;
  no requiere objeto relajado aparte). Añadidos `-Isrc/alert
  -Isrc/common/inih`.
- **`main.c`**: args `--config <path>` (default `config/procguard.ini`) y
  `--proc <base>` (default `/proc`); `now_ms` por ciclo vía
  `clock_gettime(CLOCK_MONOTONIC)`; `sleep_ms` con `nanosleep`.

Plan original de la fase (cumplido salvo lo anotado):

- `config/procguard.ini` bundled: `[policy:cpu_hog]` con
  `cpu_percent>80/60`, `persistence=3`, `cooldown=10`,
  `actions=warn,renice:10,stop,kill`; `[global]` con
  `sample_interval=500`, `sample_buffer=16` (deuda técnica), `dry_run=true`.
- Loop 10 ciclos × 500 ms: `scan → insert → cycle → store_tick → engine_gc`.
- Stdout: top-5 CPU% + decisiones del ciclo (pid, comm, policy, level,
  kind, skip_reason).

Log Slice 4 plain stderr (JSON lines llega con M7):
```
[alert] policy=<n> pid=<p> comm=<c> metric=<v> threshold=<t>
        level=<N> action=<KIND> state=<executed|dry_run|skip:<reason>>
```

## Criterios de cierre

- `make asan && make test && make lint-funclen` verdes, ~70 tests.
- `make clean && make debug && make valgrind` sin leaks.
- `./build/procguard --config config/procguard.ini` muestra decisiones
  en dry-run.
- `docs/STATE.md` actualizado; Slice 5 como siguiente.

## Deuda técnica añadida

- `AFFINITY / CAGE / TERM` parseadas pero loggean "not impl" en `act()`. Por
  ADR-014 **avanzan nivel** (no atascan) → la secuencia llega a stop/kill. El
  `cage` real (cgroups v2 + privilegios) es un slice dedicado posterior.
- Registry lineal O(N·P); aceptable < 500 procs × 5 políticas. Hash si
  hot.
- `readlink("/proc/pid/exe")` en validate puede cachearse por ciclo si
  pesa.
- Techo `max_caged_processes` → junto con `cage` (cgroups v2,
  `CAP_SYS_ADMIN`, slice posterior).
- `test_queues` secuencial; stress con `pthread_create` en Slice 5.

## Fuera de scope

`cage`, `affinity`, `term`, SIGHUP reload (Slice 8), JSON log (Slice 7),
security eval (Slice 5), red per-proceso (Slice 5).
