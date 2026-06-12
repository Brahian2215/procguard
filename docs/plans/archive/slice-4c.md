# Slice 4c — `cage` real (cgroups v2, subárbol propio)

> **CERRADO (2026-06-11, TDD).** 111 tests verdes (+8: 2 parser cage_cpu_percent
> + 1 wiring backend + 5 act cage + 2 gc cage) ASan+leak + `make valgrind`
> limpio + lint OK. Backend real `alert_cage.c` compilado/linkeado e
> inspeccionado; ruta real de cgroups validable en máquina con privilegios. El
> binario muestra CAGE en la secuencia (warn→renice→cage→stop→kill) en dry-run.
> ADR-018 registrado.
>
> **Desviaciones del plan (resueltas, ancladas):**
> - **`cage_one` con TOCTOU propio** en vez de añadir CAGE a `is_destructive()`:
>   el guard TOCTOU corre solo cuando se va a aplicar (no si el backend es NULL
>   o el techo bloquea), preservando verde el test `cage_not_impl` sin stat.
>   Comportamiento neto idéntico al plan (TOCTOU antes de cagear).
> - **Tests del parser como aserciones** dentro de `test_happy_ini`/
>   `test_global_defaults` (no funciones nuevas) — mismo cubrimiento.
> - **Sin campo `cage_cpu_percent` duplicado en el engine**: `act` lo lee de
>   `eng->global.cage_cpu_percent` (ya cargado). El engine solo añade
>   `caged`/`caged_count`/`caged_cap`.
> - **Techo de cage bloquea solo cages NUEVOS** (idempotencia): un re-cage del
>   mismo id re-aplica aunque el cupo esté lleno (no incrementa el contador).


Implementa `PG_ACT_CAGE` (hoy no-stall en `act()`): limita CPU de un proceso
escribiendo `cpu.max` en un subárbol cgroups v2 propio
`/sys/fs/cgroup/procguard/<pid>/` con mínimo privilegio (ADR-014). El backend
es **inyectable** (ADR-018, extensión de ADR-009): real por defecto, stub en
tests → unit-tests deterministas sin root. La ruta real se valida por
inspección + smoke test en máquina con privilegios (no en esta sesión).

Un commit verde por fase. TDD estricto para la lógica propia; el backend real
de sysfs es glue de I/O (validado por inspección + valgrind, sin unit-test que
requiera root).

## Decisiones (brainstorming previo)

1. **Backend inyectable** (no root para tests). Real = sysfs; stub = grabador.
2. **Solo `cpu.max`**, valor de `[global] cage_cpu_percent` (default 50).
   `cage` sigue paramless (sin tocar la lógica de params del parser).
   `memory.max` = deuda.
3. **Techo `max_caged_processes` = no-stall** (avanza a stop/kill): cage lleno =
   cage indisponible para ese proceso → AVANZA (ADR-014), no atasca la
   secuencia. Distinto del techo de kills (ADR-017, reintenta).

## Contratos nuevos

```c
/* alert.h — pg_syscalls_t extendido (append-only, ADR-018). */
typedef struct {
    int (*kill)       (pid_t, int);
    int (*setpriority)(int, id_t, int);
    int (*cage_apply) (pid_t pid, unsigned cpu_percent); /* crea+limita+adjunta */
    int (*cage_release)(pid_t pid);                       /* rmdir subárbol      */
} pg_syscalls_t;

/* alert_cage.h — backend real (sysfs cgroups v2). */
int pg_cage_apply_sysfs(pid_t pid, unsigned cpu_percent);
int pg_cage_release_sysfs(pid_t pid);

/* struct pg_alert_engine (append-only) */
pg_proc_id_t *caged;       /* ids cageados; cap = max_caged_processes */
size_t        caged_count;
size_t        caged_cap;
unsigned      cage_cpu_percent;  /* de [global] */
```

`default_syscalls()` apunta `cage_apply`/`cage_release` al backend sysfs.
`sc != NULL` con esos punteros en `NULL` (tests que solo inyectan kill/
setpriority) → `act` cae a **no-stall** (mantiene verde el test actual de CAGE).

## Fases

### Fase 1 — `cage_cpu_percent` en `[global]` (`feat(alert):`)

- `pg_global_config_t.cage_cpu_percent` (append-only; default 50).
- Parser: clave `cage_cpu_percent` en `handle_global` (uint estricto).
- Tests (test_alert_parser): `cage_cpu_percent_default_50`,
  `cage_cpu_percent_override`.

### Fase 2 — Backend real + abstracción inyectable (`feat(alert):`)

- `pg_syscalls_t` += `cage_apply`/`cage_release` (alert.h).
- `src/alert/alert_cage.{c,h}`: `pg_cage_apply_sysfs` / `pg_cage_release_sysfs`.
  - `apply`: `mkdir(/sys/fs/cgroup/procguard, 0755)` (ignora EEXIST), escribe
    `"+cpu"` a `…/procguard/cgroup.subtree_control`, `mkdir(…/procguard/<pid>)`,
    escribe `"<quota> 100000"` a `…/<pid>/cpu.max`
    (quota = `cpu_percent*1000`), escribe `<pid>` a `…/<pid>/cgroup.procs`.
    Cualquier fallo → `PG_ERR_IO` (best-effort; act no-stall).
  - `release`: `rmdir(…/procguard/<pid>)` (vacío tras muerte del proceso) →
    `PG_OK`/`PG_ERR_IO`.
  - Ruta base constante (ADR-014). Glue de I/O: sin unit-test que requiera root;
    validación por inspección + smoke en VM privilegiada.
- `default_syscalls()` los cablea. Makefile: `alert_cage.o` en test_alert_engine
  y en el binario.
- Test mínimo: `default_syscalls_wires_cage` (campos no-NULL tras init NULL sc).

### Fase 3 — `act()` ruta CAGE + registro caged (`feat(alert):`)

- Engine: alloc `caged` (cap=`max_caged_processes`) + `cage_cpu_percent` en
  `init`; free en `destroy`.
- `is_destructive()` incluye `PG_ACT_CAGE` → **guard TOCTOU** (pid reciclado se
  cancela: `skip:gone`).
- Dispatch CAGE (orden):
  1. `cage_apply == NULL` → no-stall advance (label `not_impl`).
  2. pid ya en `caged` → re-aplica (idempotente), advance (`caged`), sin
     incrementar.
  3. `caged_count >= max_caged_processes` → log `caged_ceiling`, **no-stall
     advance**.
  4. `cage_apply(pid, cage_cpu_percent)`: `PG_OK` → push a `caged`, `count++`,
     advance (`caged`); error → no-stall advance (`cage_failed`).
- Tests (stub backend grabador): `cage_applies_and_counts`,
  `cage_backend_null_no_stall` (existente, renombrado/conservado),
  `cage_idempotent_no_double_count`, `cage_ceiling_no_stall_advances`,
  `cage_toctou_mismatch_cancels`, `cage_backend_failure_no_stall`.

### Fase 4 — `engine_gc` libera cages ausentes (`feat(alert):`)

- Tras `pg_alert_state_gc`, sweep: por cada `caged[i]` ausente del store
  (`pg_store_get_history` buf_cap=1, out_len==0) → `cage_release(pid)` + saca
  del set (swap-con-último) + `count--`. Atado a la gracia G=10 (ADR-013).
- Test: `gc_releases_cage_when_absent` (caged pid fuera del store → release
  llamado, count→0); `gc_keeps_cage_present` (sigue en store → no release).

### Fase 5 — Integración + cierre (`feat(main):`)

- `config/procguard.ini`: `actions = warn, renice:10, cage, stop, kill`.
- Smoke del binario (dry-run no aplica cage real; con backend real sin
  privilegios → no-stall, secuencia progresa). Sin leaks (valgrind).
- STATE.md, slice-4c.md, memoria.

## Criterios de cierre

- `make asan && make test && make lint-funclen` verdes (+~8 tests).
- `make clean && make debug && make valgrind` sin leaks.
- Backend real compilado/linkeado e inspeccionado (smoke real gated a root).
- `docs/STATE.md` actualizado; Slice 5 / 4c-validación-privilegiada como
  siguiente.

## Deuda técnica añadida

- `memory.max` (segundo límite del cage) — futuro.
- Smoke test real de cgroups v2 (mkdir + cpu.max + cgroup.procs + verificación
  del kernel) — gated a máquina con privilegios; no corre en CI sin root.
- Cap multi-core: la fórmula `quota = cpu_percent*1000` permite `>100%` (varios
  cores) pero sin tuning ni validación específica.
- Habilitar `+cpu` en `cgroup.subtree_control` del root de cgroups depende del
  despliegue (delegación systemd); el backend lo intenta best-effort.

## Fuera de scope

`affinity`/`term` (siguen no-stall), `memory.max`, modelo de capabilities/
systemd-unit concreto (documentado en ADR-014, no codificado aquí), SIGHUP
reload (Slice 8).
