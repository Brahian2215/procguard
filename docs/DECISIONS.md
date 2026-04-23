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
