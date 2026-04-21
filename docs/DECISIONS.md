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
