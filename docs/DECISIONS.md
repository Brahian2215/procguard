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
