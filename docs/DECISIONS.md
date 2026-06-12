# Registro de Decisiones Arquitectónicas

Solo decisiones donde dos alternativas razonables competían y la elegida
tiene consecuencias propagables. Patrones aplicados (NULL checks, fail-loud,
`static` helpers, tolerancia float 1e-3 en tests, structs append-only)
viven como convenciones en `CLAUDE.md`, no aquí.

---

## ADR-001: Interfaz M1↔M2 vía loop de gobernanza, no acoplamiento directo

**Contexto:** M1 produce `pg_raw_sample_t[]`; M2 almacena por proceso. Dos
opciones: M1 llama a M2 tras cada lectura, o el loop de gobernanza itera
`pg_store_insert` sobre el array.

**Decisión:** el loop es el integrador. M1 y M2 son mutuamente ignorantes.

**Consecuencias:** tests independientes para ambos módulos, sin fixtures
cruzados. Refleja el ciclo de 10 pasos del PDF (Recolectar y Calcular son
etapas separadas conectadas por el orquestador).

---

## ADR-002: Tres hilos con mutex independientes, sin adquisición anidada

**Contexto:** separar tres temporalidades (muestreo periódico, eventos
inotify asíncronos, UI reactiva).

**Decisión:** hilo gobernanza + hilo inotify + hilo TUI. Tres mutex, ningún
hilo toma más de uno a la vez — elimina deadlocks por diseño.

**Consecuencias:** escalabilidad limitada pero robustez alta. En daemon el
hilo TUI no se instancia (2 hilos). Ver `docs/plans/slice-4-concurrency.md`.

---

## ADR-003: timestamp_ms único por scan, no por proceso

**Contexto:** cada `pg_raw_sample_t` tiene `timestamp_ms`. Podría medirse
por proceso o una sola vez por scan.

**Decisión:** una llamada a `clock_gettime(CLOCK_MONOTONIC)` al inicio del
scan; ese valor va a todas las muestras.

**Consecuencias:** evita N syscalls por scan; los deltas para CPU% se
calculan entre scans, no dentro. CLOCK_MONOTONIC es inmune a saltos NTP
(uso exclusivo para timestamps internos).

---

## ADR-004: Parser de `comm` usa el ÚLTIMO `)` como delimitador

**Contexto:** `/proc/[pid]/stat` tiene formato `pid (comm) state ...` y
`comm` puede contener espacios y paréntesis (`prctl(PR_SET_NAME)` acepta
casi cualquier byte). Parser ingenuo con primer `)` falla.

**Decisión:** `strchr(line, '(')` para el primero, `strrchr(line, ')')` para
el último. Comm es el medio; resto se parsea posicionalmente tras el último `)`.

**Consecuencias:** robusto ante nombres adversariales. Test dedicado protege
contra regresiones.

---

## ADR-005: Identificación de proceso por (pid, starttime)

**Contexto:** PIDs se reciclan. Un proceso nuevo con el PID de uno terminado
puede aparentar ser "el mismo".

**Decisión:** la tupla `pg_proc_id_t = {pid, starttime}` es la identidad.
Métricas y políticas comparan ambos campos. M4 revalida antes de actuar.

**Consecuencias:** previene acciones correctivas sobre PIDs reciclados.
Obliga a tests que cubren explícitamente el caso recycled-pid.

---

## ADR-006: Sentinel `-1.0f` para muestra inutilizable en M3

**Contexto:** `pg_metrics_cpu_percent` retorna `float`. Tres clases de
error convergen: NULL args, ID mismatch, underflow de jiffies. Alternativas:
NaN, out-param `int*`, o un único sentinel.

**Decisión:** `-1.0f` uniforme. Caller chequea `result < 0.0f`. Los tres
casos semánticamente son "no hay delta computable" — distinción más fina
no aporta al caller.

**Consecuencias:** API minimal sin `<math.h>`. El sentinel es imposible
como resultado válido (el clamp garantiza `[0.0f, 100*ncpus]`). Aplicable
como patrón a métricas float futuras (tasas I/O, bytes/s).

---

## ADR-007: Período de gracia G=10 ciclos implementado en M2, no en M1

**Contexto:** procesos que desaparecen necesitan G ciclos de retención para
que alertas pendientes completen evaluación (PDF §5.1). Quién gestiona el
contador: M1 (tracking en collector) o M2 (contador por entry en store).

**Decisión:** `pg_store_tick(store, grace)` en M2. M1 permanece stateless
para lifecycle de buffers. Alinea con ADR-001.

**Consecuencias:** M1 no necesita estado entre scans — cada scan es
independiente. M2 concentra toda la lógica temporal (buffer circular +
contador de ausencia + expiración).

---

## ADR-008: Inyección explícita de hz y ncpus en funciones de M3

**Contexto:** `pg_metrics_cpu_percent` necesita `_SC_CLK_TCK` (jiffies/s del
host) y `_SC_NPROCESSORS_ONLN` (clamp superior). Dos opciones: que M3 llame
`sysconf()` internamente, o que el integrador los obtenga una vez y los
inyecte como parámetros.

**Decisión:** parámetros explícitos `long hz`, `long ncpus`. M3 no llama
`sysconf()`.

**Consecuencias:** funciones de M3 deterministas y testeables con valores
arbitrarios sin depender del host. Tests pueden fijar `hz=100, ncpus=4` y
verificar fórmulas exactamente. Misma convención aplica a métricas futuras
de M3 que requieran constantes del host.

---

## ADR-009: Syscalls destructivas inyectadas vía struct de punteros en M4

**Contexto:** `pg_alert_engine` ejecuta acciones que tocan procesos reales
(`kill(pid, SIGSTOP)`, `kill(pid, SIGKILL)`, `setpriority`). Tests no pueden
ejercitar estas rutas sin procesos víctima ni condiciones de carrera.
Alternativas: compilar el engine en dos variantes (real y test), interceptar
vía `LD_PRELOAD`, o inyectar los syscalls como punteros.

**Decisión:** `pg_alert_engine_init` acepta opcionalmente una `pg_syscalls_t`
con punteros a `kill` y `setpriority`; por defecto apuntan a libc. Tests
inyectan stubs que cuentan invocaciones y argumentos.

**Consecuencias:** tests deterministas sin efectos laterales en el sistema.
Extiende el patrón de ADR-008 (inyección de constantes del host) a syscalls.
Coste: una indirección por llamada — irrelevante ante la frecuencia
(acciones se emiten tras `persistence` ciclos, no cada tick).

---

## ADR-010: Pipeline `evaluate → validate → act` con lista ephemeral

**Contexto:** PDF §5.10 separa los pasos 4 (evaluar rendimiento), 6 (validar)
y 7 (actuar). Opciones: (a) fusionarlos en un solo recorrido que llama
syscalls dentro del evaluador, (b) tres pases secuenciales sobre el array de
muestras con una lista intermedia de decisiones.

**Decisión:** tres pases. `evaluate()` produce `pg_alert_decision_t[]`
efímera (vive un ciclo); `validate()` anota `skip_reason` sin emitir
syscalls; `act()` dispatcha sólo las decisiones con `skip_reason==NULL`.
Dry-run vive exclusivamente en `act()`.

**Consecuencias:** cada pase es testeable aisladamente (inputs claros,
outputs observables). El dry-run no ensucia la lógica de evaluación: se
implementa cambiando dispatch en `act()` sin tocar contadores de
persistence/hysteresis. Coste: una asignación + `free` por ciclo —
amortizado contra el resto del ciclo de gobernanza.

---

## ADR-011: `risk_level` informativo, `actions` obligatorio

**Contexto:** PDF §5.4 declara un array `actions` por política (fuente de
verdad para escalamiento) y §5.5 una tabla que asocia riesgo → acción
sugerida. Si ambos se usan, una política con `risk=critical` sin `actions`
es ambigua: ¿fallback a la tabla §5.5 o error?

**Decisión:** `actions` es requerido. `risk_level` es metadato para logs y
TUI, sin efecto en el engine. Parser rechaza políticas sin `actions`.

**Consecuencias:** una sola fuente de verdad para escalamiento (el array
`actions[]`). Elimina código de fallback que sería difícil de probar. El
admin tiene que declarar explícitamente qué hace el engine — alinea con
filosofía fail-loud del proyecto.

---

## ADR-012: Whitelist dinámica vía `ppid == own_pid` en runtime

**Contexto:** ProcGuard lanza hijos (ej. reporter M7, próximos subprocesos
de cgroups en 4b). Matarlos accidentalmente sería catastrófico. Opciones:
(a) API de registro explícito (`pg_alert_register_child(pid)`), (b) chequeo
runtime de `sample->ppid == engine->own_pid`.

**Decisión:** runtime. El whitelist se computa por ciclo comparando
`sample->ppid` contra `own_pid` (capturado en `engine_init`).

**Consecuencias:** cubre hijos futuros sin necesidad de hooks en cada
`fork()`. No hay estado mutable de registro (todo derivado del sample).
Trade-off: un `ppid` reciclado por el kernel cuando el hijo muere y otro
proceso hereda esa relación padre–hijo podría aparentar ser hijo; en
práctica Linux solo pone `ppid=1` (reparenting a init) tras muerte del
padre, así que el falso positivo es imposible mientras ProcGuard viva.

---

## ADR-013: Freeze de contadores M4 durante ausencia; GC alineado con M2

**Contexto:** cuando un proceso no aparece en el scan actual (terminó,
permission denied intermitente), los contadores `persistence` y
`hysteresis` del `pg_alert_state_t` correspondiente necesitan una política.
Opciones: (a) reset a 0 (proceso "nuevo"), (b) incremento de una ausencia
counter paralela, (c) freeze (no tocar).

**Decisión:** freeze. Nadie toca el state si no hay sample en el ciclo. La
vida útil del state está atada al lifecycle de M2: `pg_alert_engine_gc()`
se invoca tras `pg_store_tick()` y libera entries cuyo `id` ya no está en
el store (M2 ya aplicó el período de gracia G=10 de ADR-007).

**Consecuencias:** M4 no reimplementa contador de ausencia — delega en la
gracia de M2. Si un proceso desaparece 3 ciclos y vuelve, sus contadores
siguen donde estaban (correcto: es el mismo proceso por `(pid, starttime)`).
Si desaparece >G=10 ciclos, M2 lo expira y el siguiente `gc` libera el
state. Orden obligatorio en el integrador: `engine_cycle → store_tick →
engine_gc`.

---

## ADR-014: Acción no disponible AVANZA el escalamiento (no lo atasca); cage real con mínimo privilegio

**Contexto:** la secuencia de escalamiento del PDF (warn→renice→cage→stop→
kill) incluye `cage` (cgroups v2), `affinity` y `term`. Un primer plan dejaba
estas acciones como "log: not implemented" **sin avanzar nivel** — pero eso
**atasca** el escalamiento: si la política llega a `cage` y cage no está
disponible (sin privilegios, o aún no implementado), nunca alcanza `stop`/
`kill`. Esto sabotea la métrica de "efectividad de gobernanza" del experimento
(§10.1): las anomalías de CPU no se resolverían porque `renice` solo es
efectivo bajo contención y `cage` no correría. Además, ejecutar las acciones
destructivas requiere privilegios elevados, lo que abre superficie de riesgo.

**Decisión:** dos partes.
1. **No-stall:** una acción no disponible o no implementada en `act()`
   (`cage`/`affinity`/`term` mientras no estén) se registra y **avanza el
   nivel de escalamiento** igual que una acción ejecutada (mismo efecto sobre
   `cooldown_until_ms` y reset de `persistence`). El escalamiento nunca se
   atrapa; siempre puede llegar a `stop`/`kill`.
2. **Cage real con mínimo privilegio:** cuando se implemente (slice dedicado,
   no 4b), `cage` escribe `cpu.max`/`memory.max` **solo** bajo
   `/sys/fs/cgroup/procguard/<pid>/`, nunca toca cgroups del sistema. Se
   prefiere **delegación de cgroups de systemd** (`Delegate=yes`) o
   capabilities acotadas (`CAP_KILL`, `CAP_SYS_NICE`, `CAP_SYS_ADMIN` solo
   para el subárbol) sobre root pleno. Si la creación del cgroup falla en
   runtime, aplica la regla no-stall (avanza nivel, registra el fallo).

**Consecuencias:** el contención de daño se apoya en las protecciones ya
especificadas (PDF §7, ADR-005/012): `dry_run=true` por defecto, lista
blanca inmutable, techo de acciones, cordura 5s, reválida `(pid,starttime)`.
Root/capabilities solo amplifican el alcance de acciones que esas barreras
ya filtran. El experimento mide una secuencia que SIEMPRE progresa, así que
la efectividad de gobernanza no queda artificialmente en cero por una acción
intermedia indisponible. Coste: `cage` real es un slice propio (cgroups v2 +
modelo de privilegios); 4b solo implementa el no-stall y warn/renice/stop/
kill.

---

## ADR-015: Concurrencia de Slice 5 se valida bajo ThreadSanitizer, no solo ASan

**Contexto:** las tres colas IPC (ADR-002) se testean hoy de forma
**secuencial**. ASan+UBSAN —los sanitizers del proyecto— **no detectan data
races**. Cuando Slice 5 instancie los hilos reales (gobernanza, inotify,
TUI), un race sobre las estructuras compartidas pasaría los tests verdes y
mordería en la demo.

**Decisión:** Slice 5 añade validación bajo **ThreadSanitizer**. El target
`make tsan` ya existe como scaffold (compila el binario con
`-fsanitize=thread`; TSan y ASan no conviven, igual que valgrind). Slice 5
añade variantes de test con `pthread_create` que estresan las colas
productor/consumidor y se ejecutan bajo este target. Criterio de cierre de
Slice 5: tests con hilos verdes bajo `make tsan`, además de ASan/valgrind
para la lógica secuencial.

**Consecuencias:** los races se cazan antes de la integración, no en la
demo. Coste: un árbol de objetos paralelo para tests-bajo-tsan (TSan no
mezcla con los .o compilados con ASan) — se construye en Slice 5 cuando
exista el primer test con hilos, no antes (scope discipline).

---

## ADR-016: `exe_path` se recolecta en M1; revalidación TOCTOU de `(pid,starttime)` en `act()`

**Contexto:** el whitelist de M4 `validate` (ADR-012, PDF §7) necesita la
ruta real del ejecutable (`/proc/[pid]/exe`) para dos reglas: "protegido por
nombre **y** ruta estándar" y detección de kernel thread (`exe==""`). El
`pg_raw_sample_t` no la trae y M1 no la lee. Además, la revalidación de
`(pid,starttime)` "vs el sample actual" dentro de un ciclo síncrono
(evaluate→validate→act) es un no-op: evaluate ya usó ese mismo sample, así
que el starttime coincide por construcción. La protección real contra PID
reciclado (ADR-005) es la ventana TOCTOU entre leer procfs y enviar el
syscall.

**Decisión:** tres partes.
1. **`exe_path` vive en el sample (M1).** Por ADR-001 (M1 es el único dueño
   de la lectura de procfs), el collector hace `readlink(/proc/[pid]/exe)` y
   lo guarda en `pg_raw_sample_t.exe_path` (campo append-only, bounded
   `PG_EXE_MAX`; truncado documentado). Kernel threads / procesos muertos →
   `exe_path==""`. `validate` permanece **puro** (sin I/O), operando sobre el
   sample. Reutilizado por M5 `disguised_process`.
2. **TOCTOU en `act()`, no en `validate`.** La revalidación autoritativa
   re-lee el starttime actual del pid **justo antes** del syscall, vía un
   helper de M1 `pg_collector_read_starttime(proc_base, pid, *out)`. Si
   cambió o el proceso desapareció → se cancela la acción y se registra
   "proceso desaparecido antes de acción". `validate` conserva solo un
   chequeo barato de consistencia (decisión.id vs sample del ciclo).
3. **`engine_init` recibe `proc_base`.** Necesario para el re-read de (2).
   Append a la firma: `pg_alert_engine_init(eng, ini_path, proc_base,
   own_pid, hz, ncpus, sc)`.

**Consecuencias:** M4 no lee procfs (ADR-001 intacto); `validate` y `act`
siguen testeables en aislamiento (validate puro sobre samples; act re-lee
vía helper inyectable/fixture). El `exe_path` no se desperdicia: M5 lo
necesita. Coste: M1 crece (readlink + helper starttime, sub-fase 5a con
TDD); cada sample carga `PG_EXE_MAX` bytes extra (aceptable < cientos de
procs).

---

## ADR-017: Modelo de ejecución de `act()` — techo transitorio, no-stall, dry-run sin TOCTOU

**Contexto:** `act()` (Slice 4b Fase 6) aplica las decisiones que sobreviven a
`validate`. Tres puntos del flujo admitían dos lecturas razonables, y la
elección de cada uno tiene consecuencias sobre el escalamiento y el
experimento (§10):

1. **Techo kills/min alcanzado:** ¿la decisión KILL bloqueada por el techo
   AVANZA el nivel (como una acción ejecutada) o se trata como un skip que se
   reintenta?
2. **Acción no implementada (AFFINITY/CAGE/TERM):** ya cubierto por ADR-014
   (avanza), pero conviene contrastarlo con (1).
3. **Dry-run y el guard TOCTOU:** ¿el modo previsualización re-lee el starttime
   real (TOCTOU) aunque no ejecute syscall?

**Decisión:**

1. **El techo es un freno transitorio, NO avanza.** KILL con
   `kills_last_minute >= max_kills_per_minute` se registra `skip:ceiling`, no
   ejecuta syscall, **no** fija cooldown, **no** resetea persistence y **no**
   avanza nivel. El próximo ciclo lo reemite y se reintenta en cuanto se libere
   cupo en la ventana de 60 s. Avanzar habría llevado el nivel más allá de
   `kill` (a "exhausted") dejando el proceso vivo: derrota el propósito del
   freno. El techo gobierna la *tasa* de kills, no la *secuencia*.
2. **El no-stall sí avanza** (ADR-014): una acción intermedia indisponible
   (`cage`/`affinity`/`term`) cuenta como ejecutada para que la secuencia
   siempre alcance `stop`/`kill`. Eje ortogonal al techo: indisponibilidad de
   *acción* (avanza) vs límite de *tasa* (reintenta).
3. **El dry-run no hace TOCTOU.** No hay syscall destructivo que proteger, así
   que el modo previsualización omite el re-read de starttime, loguea
   `state=dry_run` y avanza nivel/cooldown igual que una ejecución. Mantiene la
   previsualización de la secuencia completa independiente del estado vivo del
   proceso (y de un procfs montado en tests). El guard TOCTOU (ADR-016) opera
   solo en modo real, justo antes de RENICE/STOP/KILL.

**Consecuencias:** el log distingue cuatro estados terminales por decisión:
`executed`, `dry_run`, `skip:ceiling` (reintentable) y `skip:gone` (TOCTOU
falló: PID reciclado o desaparecido). Solo `executed`/`dry_run` mutan el state
(cooldown + reset persistence + avance/reactivación). El experimento mide una
secuencia que progresa salvo cuando el techo la frena deliberadamente — la
efectividad de gobernanza no queda en cero por una acción intermedia
indisponible, pero el techo sí acota el daño por unidad de tiempo. El contador
`max_caged_processes` y su campo en el engine se difieren a Slice 4c (cage
real): ninguna acción de 4b lo lee/escribe, añadirlo ahora sería campo muerto.

---

## ADR-018: Backend de `cage` inyectable vía `pg_syscalls_t`; real = sysfs cgroups v2 subárbol propio

**Contexto:** Slice 4c implementa `PG_ACT_CAGE` (hasta 4b era no-stall puro).
El cage limita CPU escribiendo `cpu.max` en un subárbol cgroups v2
(`/sys/fs/cgroup/procguard/<pid>/`, ADR-014). Esa escritura requiere privilegios
(root, capabilities acotadas o delegación systemd) que no existen en una sesión
de usuario ni en CI. Igual que las syscalls destructivas (ADR-009), los tests
no pueden ejercitar la ruta real sin efectos colaterales ni privilegios.

**Decisión:** el "cage backend" son dos punteros a función añadidos a
`pg_syscalls_t` (struct ya inyectable de ADR-009): `cage_apply(pid,
cpu_percent)` y `cage_release(pid)`. Por defecto (`sc == NULL` en
`engine_init`) apuntan al backend real de sysfs (`pg_cage_apply_sysfs` /
`pg_cage_release_sysfs`). Los tests inyectan stubs grabadores. Si un `sc`
inyectado deja esos punteros en `NULL` (test que solo inyecta kill/setpriority),
`act()` trata CAGE como **no-stall** (avanza sin cagear) — degradación elegante.

Tres reglas de ejecución de CAGE en `act()`:
1. **CAGE es destructive → guard TOCTOU** (ADR-016): re-lee el starttime antes
   de adjuntar el pid; reciclado/ausente → cancela (`skip:gone`). Cagear el pid
   equivocado es tan grave como matarlo.
2. **Techo `max_caged_processes` = no-stall** (no reintenta, a diferencia del
   techo de kills de ADR-017): cage lleno = cage indisponible para ese proceso
   → AVANZA a stop/kill (ADR-014). `cage` es intermedio en la secuencia
   (warn→renice→cage→stop→kill); reintener atascaría el escalamiento.
3. **Cualquier fallo del backend** (sin privilegios, mkdir/write falla) →
   no-stall advance (`cage_failed`). El ciclo nunca aborta.

El engine lleva un registro de ids cageados (`caged[]`, cap =
`max_caged_processes`) para: idempotencia (no doble-contar re-cage), el techo, y
la liberación. `engine_gc` libera (`rmdir`) los cages cuyo proceso ya no está en
el store, atado a la gracia G=10 de M2 (ADR-013).

**Consecuencias:** unit-tests deterministas de toda la lógica de cage sin root;
el backend real se implementa e inspecciona y se valida con un smoke test
gated a privilegios (no en CI). La contención de daño se apoya en las 6 capas
ya especificadas (PDF §7): el cage real solo amplía el alcance de acciones que
whitelist/techo/cordura/reválida/dry_run ya filtran. `memory.max` (segundo
límite) queda como deuda; `affinity`/`term` siguen no-stall.
