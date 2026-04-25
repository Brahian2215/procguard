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

Itera `samples × catalog`. Para métricas con delta, obtiene `prev` vía
`pg_store_get_history(store, id, hist, 2, &hlen)`. Estados sin sample →
freeze (ADR-013). State machine PDF §5.4:

1. `type==SECURITY` → skip (activa en Slice 5).
2. Métrica sentinel `-1.0f` → freeze.
3. `metric > threshold` → `persistence++`, `hysteresis=0`. Primera vez que
   `persistence == policy.persistence` y `alert_active_since_ms==0` →
   setear a `now`.
4. `metric < threshold_low` → `hysteresis++`.
5. Emitir decisión si `persistence >= policy.persistence` y
   `now >= cooldown_until_ms`.
6. Desactivación por histéresis: reset contadores,
   `alert_active_since_ms=0`, `deactivated_since_last_act=true`. **No**
   decrementa `escalation_level`.
7. `escalation_level >= n_actions` → log "exhausted", sin nueva decisión.

`act()` aplica el avance: si `deactivated_since_last_act==false` →
`level++`; si `true` → mismo nivel (reactivación). En ambos casos
`cooldown_until_ms = now + cooldown_s*1000`, reset persistence, flag a
false.

Tests mínimos (≥10): `above_threshold_persistence++`,
`below_low_hysteresis++`, `absent_freezes`, `sentinel_freezes`,
`persistence_reached_emits`, `persistence_1_immediate`, `cooldown_blocks`,
`cooldown_0_immediate`, `hysteresis_deactivates_preserving_level`,
`reactivation_same_level`, `post_cooldown_advances_level`,
`full_escalation_warn_renice_stop_kill`, `exhausted_no_new_decision`,
`multiple_policies_same_proc`, `security_skipped`.

### Fase 5 — `validate` (`feat(alert):`)

Por decisión, en orden (primer hit gana):

1. **Reválida starttime** vs sample actual → skip `stale_id` (ADR-005).
2. **Whitelist** (ADR-012): `pid==1`, `pid==own_pid`, `ppid==own_pid`,
   `comm∈protected_names ∧ exe∈{/usr/bin,/usr/sbin,/bin,/sbin}`, kernel
   thread (`readlink(exe)==""`) → skip `protected`.
3. **Techos**: `KILL` y `kills_last_minute >= max_kills_per_minute` → skip
   `ceiling`. `max_caged_processes` queda para cuando entre `cage`.
4. **Cordura 5s** (PDF §7): `{STOP, KILL}` y
   `now - alert_active_since_ms < 5000` → skip `sanity`.

Tests (≥9): `whitelist_pid_1`, `whitelist_own_pid`,
`whitelist_child_by_ppid`, `whitelist_protected_std_path`,
`whitelist_protected_nonstd_rejected`, `whitelist_kernel_thread`,
`stale_starttime_skipped`, `sanity_5s_blocks_kill`,
`ceiling_kills_per_minute`.

### Fase 6 — `act` (`feat(alert):`)

**Log primero** (todas las decisiones, incluyendo skips y dry-run).
Dispatch sólo para `skip_reason==NULL`:

- `WARN` → log.
- `RENICE` → `syscalls->setpriority(PRIO_PROCESS, pid, param)`.
- `STOP`  → `syscalls->kill(pid, SIGSTOP)`.
- `KILL`  → `syscalls->kill(pid, SIGKILL)` + push ring kills/min.
- `AFFINITY / CAGE / TERM` → log "not yet implemented"; **no** avanza
  nivel ni toca cooldown (el mismo nivel reaparecerá hasta editar INI).

**Dry-run** (`cfg.dry_run==true`): loggea `[DRY-RUN] would <kind>` pero
**sí** avanza nivel/cooldown (para ver la secuencia antes de dar poder).

Tras ACT ejecutada o dry-run: `cooldown_until_ms = now + cooldown_s*1000`,
reset persistence; avance según `deactivated_since_last_act` (Fase 4).

Tests (≥8): `warn_no_syscall`, `renice_calls_setpriority`,
`stop_calls_kill_sigstop`, `kill_sigkill_and_pushes_ring`,
`cage_logs_not_impl_no_syscall`, `dry_run_suppresses_syscalls_advances_level`,
`skip_protected_is_logged`, `escalation_advance_vs_reactivation`.

### Fase 7 — Integración `main.c` (`feat(main):`)

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

- `AFFINITY / CAGE / TERM` parseadas pero skip en `act()` → escalamiento
  atrapado hasta reeditar INI. Documentar prominente en STATE.md.
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
