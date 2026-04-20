# Registro de Decisiones Arquitectónicas

Cada decisión tiene: contexto, decisión, consecuencias.

## ADR-001: Lenguaje C11
**Contexto:** requisito académico y de rendimiento.
**Decisión:** C estándar C11, sin extensiones GNU salvo `_GNU_SOURCE`
para API de glibc donde sea necesario.
**Consecuencias:** portabilidad limitada a Linux, pero el proyecto es
Linux-specific por diseño.

## ADR-003: Interfaz M1→M2 a través del loop de gobernanza
**Contexto:** M1 (collector) devuelve un array plano de pg_raw_sample_t. M2
(sample store) necesita insertar esas muestras en buffers circulares por proceso.
**Decisión:** El loop de gobernanza (paso 2→3 del ciclo) es el integrador: recibe
el array de M1 e itera llamando pg_store_insert(store, &sample) por cada entrada.
M1 y M2 son mutuamente ignorantes; no existe dependencia M1→M2 ni M2→M1.
**Consecuencias:** M1 y M2 pueden testearse de forma completamente independiente.
El governance loop es el único que conoce ambos módulos. Refleja el ciclo de 10
pasos del PDF donde "Recolectar" y "Calcular" son etapas separadas conectadas por
el orquestador, no por los módulos entre sí.

## ADR-002: Tres hilos fijos
**Contexto:** necesidad de separar temporalidades (ciclo regular, eventos
asíncronos inotify, UI reactiva).
**Decisión:** tres hilos con mutex independientes, sin adquisición
anidada para eliminar deadlocks por diseño.
**Consecuencias:** escalabilidad limitada pero robustez alta. Apropiado
para el alcance del proyecto.

## ADR-004: timestamp_ms único por scan (no por proceso)
**Contexto:** `pg_collector_scan` produce N muestras `pg_raw_sample_t`; cada una
tiene un campo `timestamp_ms`. Hay dos opciones: una medición global del scan o
una por cada proceso justo antes de leer su `stat`.
**Decisión:** una sola llamada a `clock_gettime(CLOCK_MONOTONIC)` al inicio del
scan; ese valor se asigna a todas las muestras del scan.
**Consecuencias:** semánticamente "todas las muestras pertenecen al mismo
instante". Simplifica el cálculo de deltas para CPU% (Sesión 2: dos scans,
mismo proceso, restar utime+stime / restar timestamps). Evita N syscalls
extra. La precisión sub-scan se pierde, pero esa precisión nunca era real:
los procesos al final del scan ya habían avanzado respecto al inicio del scan.

## ADR-005: API pública es defensive ante args NULL (PG_ERR_PARSE)
**Contexto:** las tres funciones públicas del collector reciben punteros del
caller. Un NULL es un error programático.
**Decisión:** `pg_collector_init` y `pg_collector_scan` validan punteros NULL
y retornan `PG_ERR_PARSE`. `pg_collector_destroy(NULL)` es no-op (idiomático
estilo `free`).
**Consecuencias:** API pública robusta también en builds release. `assert()`
no protegería en release builds (`-DNDEBUG` lo desactiva). El coste por call
es un compare-and-branch. Aplicable a todas las APIs públicas de módulos
futuros.

## ADR-006: Parser de `comm` usa el ÚLTIMO ')' como delimitador
**Contexto:** `/proc/[pid]/stat` formato: `pid (comm) state ...`. El campo
`comm` puede contener espacios, paréntesis y caracteres especiales (Linux
permite `prctl(PR_SET_NAME)` con casi cualquier byte). Un parser ingenuo
que use el primer `)` falla con nombres como `weird ) name`.
**Decisión:** usar `strchr(line, '(')` para el primer `(` y `strrchr(line, ')')`
para el último `)`. El comm es lo de en medio.
**Consecuencias:** robusto contra nombres de proceso adversariales. Test
`parses_comm_with_internal_parens` protege contra regresiones. El resto
de campos (state, ppid, ..., starttime) se parsean con `sscanf` posicional
desde el byte siguiente al último `)`.

## ADR-007: `vmrss` y otros campos de memoria diferidos a Slice 2
**Contexto:** `pg_raw_sample_t` representa una muestra cruda. `vmrss` y RSS
en general viven en `/proc/[pid]/statm`, no en `stat`.
**Decisión:** Slice 1 sólo lee `stat`. Los campos de memoria entran cuando
Slice 2 introduzca métricas de memoria.
**Consecuencias:** `pg_raw_sample_t` crecerá en Slice 2 (cambio de struct, no
de API). Como aún no hay M2 (Sample Store) que dependa del layout, no rompe
nada. `comm` se reserva como `[256]` (no `[16]` que sería el límite real
de Linux) por holgura defensiva contra fixtures sintéticos largos.

## ADR-008: `CLOCK_MONOTONIC` para todos los timestamps
**Contexto:** existen `gettimeofday`, `time()`, `clock_gettime(CLOCK_REALTIME)`
y `clock_gettime(CLOCK_MONOTONIC)`. Necesitamos timestamps para deltas de CPU%.
**Decisión:** `CLOCK_MONOTONIC` exclusivamente para cualquier campo
`timestamp_ms` interno del sistema.
**Consecuencias:** inmune a saltos por NTP, cambios de zona horaria o ajustes
manuales del reloj. `CLOCK_MONOTONIC` no es comparable entre boots ni entre
máquinas, pero esa comparación nunca se requiere — sólo deltas dentro del
mismo proceso. Para timestamps mostrados al usuario (logs JSON, etc.), Slice 4+
usará `CLOCK_REALTIME` por separado.

## ADR-009: `pg_collector_t` es type opaco desde el inicio
**Contexto:** la struct interna del collector hoy sólo contiene `char *proc_base`.
Podría exponerse directamente como struct pública.
**Decisión:** declararla como type opaco (`typedef struct pg_collector
pg_collector_t;` en el header, definición completa en el `.c`).
**Consecuencias:** permite añadir estado interno (tracking de procesos para
gracia G=10, caché del último scan, mutex futuros) sin romper la API ni el ABI
de los callers. Coste actual: una indirección extra al acceder al estado interno
(despreciable).

## ADR-010: `pg_metrics_cpu_percent` recibe `ncpus` como parámetro
**Contexto:** el clamp superior del resultado es `100 * ncpus`. Se podría
consultar `sysconf(_SC_NPROCESSORS_ONLN)` internamente en cada llamada o
inyectarlo como parámetro por simetría con `hz`.
**Decisión:** firma `pg_metrics_cpu_percent(prev, curr, hz, ncpus)`. El caller
(loop de gobernanza / main de Slice 1) consulta `sysconf` una vez y propaga.
**Consecuencias:** tests deterministas — no dependen del núcleo de CPUs del
host. Sin syscall en hot-path. Coherente con la filosofía de M3 como cálculo
puro. El caller asume la responsabilidad de pasar un `ncpus` sensato; M3 aplica
`max(ncpus, 1)` como red de seguridad defensiva.

## ADR-011: Sentinel único `-1.0f` para "muestra inutilizable" en M3
**Contexto:** M3 devuelve `float`. ADR-005 establece que las APIs públicas
deben detectar NULL. Hay tres clases de error convergentes: NULL args, ID
mismatch (pid o starttime distintos) y violación de monotonía. Se consideró
NaN, cambiar a firma `int*` con out-param, o un único sentinel.
**Decisión:** `-1.0f` es el único valor de error. Caller verifica `result <
0.0f`. Aplica a (a) prev o curr NULL, (b) ID mismatch, (c) underflow
jiffies.
**Consecuencias:** API mínima y sin dependencia de `<math.h>`. El valor
`-1.0f` es imposible como resultado válido porque M3 clampa a `[0.0f,
100*ncpus]`. Semánticamente todos los casos son "no hay delta computable con
estas dos muestras" — una distinción más fina no aporta al caller de Slice 1
(main, luego governance loop): ambas respuestas son "salta este proceso en
esta iteración".

## ADR-012: Chequeo explícito de underflow `utime+stime` en M3
**Contexto:** `delta_cpu = (curr->utime+stime) - (prev->utime+stime)` con
aritmética `unsigned long long`. Si por alguna razón `prev > curr` (pid
reciclado con mismo starttime por casualidad astronómica, corrupción de M1,
bug futuro) el resultado hace wrap a un número enorme y el clamp final lo
lleva a `100 * ncpus` en lugar de a 0 — falsa alarma de saturación.
**Decisión:** comprobación explícita antes de restar; retorna -1.0f si
`curr_cpu < prev_cpu`. `timestamp_ms` (CLOCK_MONOTONIC, mismo boot) no
requiere el mismo chequeo porque el kernel garantiza monotonía (ADR-008).
**Consecuencias:** un compare-and-branch extra; robustez ante violaciones de
invariante de módulos aguas arriba. Test `underflow_returns_sentinel`
protege contra regresiones. Principio general aplicable a futuras métricas
con aritmética unsigned sobre deltas (I/O rates en Slice 2, etc.).

## ADR-013: Tolerancia de tests float = 1e-3 via `TEST_ASSERT_FLOAT_WITHIN`
**Contexto:** la fórmula de CPU% combina aritmética `double` con cast final a
`float`. `TEST_ASSERT_EQUAL_FLOAT` (tolerancia ~1 ULP / 1.19e-7) funciona
para los tests actuales pero es frágil ante reordenamientos del cálculo.
**Decisión:** todos los tests de M3 usan `TEST_ASSERT_FLOAT_WITHIN(0.001f,
expected, actual)`. Tolerancia 1e-3 es tres órdenes de magnitud menor que la
granularidad operacional de un porcentaje (0.1%).
**Consecuencias:** los tests sobreviven a cambios razonables del cálculo
interno (reordenar multiplicaciones, cachear `max_pct`, etc.). Política
aplicable por defecto a futuras métricas float (tasas I/O, bytes por
segundo).
