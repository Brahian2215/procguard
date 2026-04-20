# Slice 1: M1 (Data Collector) + M3 CPU% + binario ejecutable

## Objetivo

`./build/procguard` compila limpio (-Wall -Wextra -Werror), muestra top-5 procesos
por CPU% en stdout, y pasa `make test` con ASAN sin errores.
Establece el patrón TDD y el Makefile funcional para todos los módulos siguientes.

**Nota de alcance**: main.c en este slice es un integrador temporal (2 scans lineales
+ print). Será reemplazado por completo en Slice 2 cuando entren hilos y config.
El valor de este slice es M1 y M3 con sus tests, no el main.c.

---

## Sesión 1 — Tipos + Collector + Makefile funcional

### 1. `src/common/pg_types.h`

```c
#ifndef PG_TYPES_H
#define PG_TYPES_H

#include <sys/types.h>

typedef struct {
    pid_t              pid;
    unsigned long long starttime;
} pg_proc_id_t;

typedef struct {
    pg_proc_id_t       id;
    char               comm[256];
    char               state;
    long               ppid;
    int                tty_nr;
    unsigned long long utime;        /* jiffies en modo usuario */
    unsigned long long stime;        /* jiffies en modo kernel */
    unsigned long long timestamp_ms; /* CLOCK_MONOTONIC en ms al momento del scan */
} pg_raw_sample_t;

/* vmrss se introduce en Slice 2 (requiere leer /proc/[pid]/statm por separado) */

#define PG_GRACE_CYCLES 10

#define PG_OK        0
#define PG_ERR_IO   -1
#define PG_ERR_PARSE -2
#define PG_ERR_MEM  -3

#endif /* PG_TYPES_H */
```

**Decisión**: `vmrss` excluido de Slice 1 — requiere abrir `/proc/[pid]/statm`
(archivo separado de stat), lo que agrega complejidad al colector sin aportar
al objetivo CPU% de este slice. Se introduce en Slice 2.

**Decisión**: `timestamp_ms` usa `CLOCK_MONOTONIC` (no `gettimeofday`). Es inmune
a ajustes de NTP y cambios de zona horaria. Para deltas de CPU es el clock correcto.

### 2. `src/collector/collector.h`

```c
#ifndef PG_COLLECTOR_H
#define PG_COLLECTOR_H

#include "pg_types.h"
#include <stddef.h>

typedef struct pg_collector pg_collector_t; /* opaco */

/*
 * Inicializa el colector. proc_base es la raíz de procfs (normalmente "/proc").
 * Retorna PG_OK o PG_ERR_MEM si falla la alocación interna.
 */
int pg_collector_init(pg_collector_t **col, const char *proc_base);

/*
 * Escanea proc_base y retorna un array alocado con las muestras crudas.
 * *out debe ser liberado por el caller con free(*out).
 * Retorna PG_OK, PG_ERR_IO (proc_base inaccesible) o PG_ERR_MEM.
 * Errores por proceso individual son silenciosos (best-effort).
 */
int pg_collector_scan(pg_collector_t *col,
                      pg_raw_sample_t **out, size_t *out_count);

/*
 * Libera recursos del colector. Seguro llamar con col==NULL.
 */
void pg_collector_destroy(pg_collector_t *col);

#endif /* PG_COLLECTOR_H */
```

### 3. `src/collector/collector.c`

Lógica de parsing de `/proc/[pid]/stat`:
- Leer la línea completa en buffer
- El campo `comm` está entre el primer `(` y el **último** `)` de la línea
  (el nombre puede contener espacios, paréntesis, y caracteres especiales)
- Después del último `)` hay un espacio y los campos restantes en orden posicional
- Parsear con sscanf los campos: state(1), ppid(2), ... utime(11), stime(12),
  ... starttime(19) — relativo al inicio del stat post-comm

`timestamp_ms` se obtiene con `clock_gettime(CLOCK_MONOTONIC)` antes de abrir
el archivo stat del proceso.

Si `opendir(proc_base)` falla → retornar `PG_ERR_IO`.
Si `malloc` para el array de salida falla → retornar `PG_ERR_MEM`.
Si un proceso individual falla (ya terminó, permisos) → skip silencioso.

### 4. `tests/unit/test_collector.c`

Formato del stat sintético (campos mínimos, resto en 0):
```
100 (bash) S 1 100 100 0 -1 0 0 0 0 0 150 50 0 0 20 0 1 0 12345 0 0 ...
```
Campos en orden post-comm: state ppid pgrp session tty_nr tpgid flags
minflt cminflt majflt cmajflt utime stime cutime cstime priority nice
num_threads itrealvalue starttime ...

**Fixture** (`setUp`): crear `/tmp/pg_test_proc/` con:
- `100/stat`: pid=100, comm=(bash), state=S, ppid=1, utime=150, stime=50, starttime=12345, tty_nr=0
- `200/stat`: pid=200, comm=(nginx), state=S, ppid=1, utime=200, stime=100, starttime=67890, tty_nr=0
- `300/stat`: pid=300, comm=(python3), state=R, ppid=100, utime=500, stime=200, starttime=11111, tty_nr=0

**tearDown**: `rm -rf /tmp/pg_test_proc/`

**Tests**:

```
TEST(scan_finds_all_three)
  scan → out_count == 3
  los tres pids (100, 200, 300) están presentes en out[]

TEST(disappeared_process)
  scan #1 → out_count == 3
  eliminar directorio 200/
  scan #2 → out_count == 2
  pid 200 no aparece en out[]

TEST(recycled_pid)
  scan #1 → entry con id={pid=100, starttime=12345}
  reescribir 100/stat con starttime=99999
  scan #2 → entry con id={pid=100, starttime=99999}
  las dos entradas tienen MISMO pid pero DISTINTO starttime → pg_proc_id_t distintos

TEST(malformed_stat_is_skipped)
  crear 400/stat con contenido vacío ""
  scan → out_count == 3 (400 skipeado), no crash, no leak
```

### 5. Makefile — cambios exactos a aplicar

Reemplazar el target `test` placeholder y agregar las reglas de build:

```makefile
UNITY_DIR     := tests/unity
TEST_UNIT_DIR := tests/unit

$(BUILD_DIR):
	mkdir -p $@

# Unity: vendored third-party — compilar sin -Werror
$(BUILD_DIR)/unity.o: $(UNITY_DIR)/unity.c | $(BUILD_DIR)
	$(CC) -std=c11 -O1 -g3 -I$(UNITY_DIR) -c $< -o $@

# test_collector: fuentes compiladas inline con ASAN
# (evita conflicto de .o entre build normal y build de test)
$(BUILD_DIR)/test_collector: $(TEST_UNIT_DIR)/test_collector.c \
		src/collector/collector.c $(BUILD_DIR)/unity.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CFLAGS_ASAN) \
	  -I$(UNITY_DIR) -Isrc/common -Isrc/collector \
	  $^ -o $@ $(LDFLAGS) -fsanitize=address -fsanitize=undefined

TEST_BINS := $(BUILD_DIR)/test_collector

test: $(TEST_BINS)
	@failed=0; \
	for t in $(TEST_BINS); do \
		echo "=== $$t ==="; \
		$$t || failed=1; \
	done; \
	exit $$failed

valgrind: $(BUILD_DIR)/procguard
	valgrind --leak-check=full --error-exitcode=1 $(BUILD_DIR)/procguard

# Stub main: desbloquea make debug/asan hasta que main.c sea real
$(BUILD_DIR)/procguard: src/collector/collector.c src/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Isrc/common -Isrc/collector $^ -o $@ $(LDFLAGS) $(LDLIBS)
```

**Deuda técnica conocida**: compilar las fuentes inline en cada test binary es
correcto para Slice 1 pero no escala. En Slice 3, cuando haya 2+ módulos con
tests, se introduce `build/tests/` con objetos compilados con ASAN reutilizables.

**Stub `src/main.c`** para esta sesión:
```c
#include <stdio.h>
int main(void) {
    fprintf(stderr, "procguard: not yet implemented\n");
    return 1;
}
```
Produce binario que falla explícitamente (no silencioso). Desbloquea `make debug`.

---

## Sesión 2 — Metrics CPU% + tests

### 6. `src/metrics/metrics.h`

```c
#ifndef PG_METRICS_H
#define PG_METRICS_H

#include "pg_types.h"

/*
 * Calcula el porcentaje de CPU usado entre dos muestras del mismo proceso.
 * hz: sysconf(_SC_CLK_TCK) — pasado como parámetro para testabilidad.
 *
 * Retorna -1.0f si los IDs no coinciden (proceso reciclado entre muestras).
 * Retorna  0.0f si elapsed_ms == 0 (evita división por cero).
 * El resultado está clampeado a [0.0, 100.0 * num_cpus].
 */
float pg_metrics_cpu_percent(const pg_raw_sample_t *prev,
                              const pg_raw_sample_t *curr,
                              long hz);

#endif /* PG_METRICS_H */
```

### 7. `src/metrics/metrics.c` — fórmula exacta

```c
float pg_metrics_cpu_percent(const pg_raw_sample_t *prev,
                              const pg_raw_sample_t *curr,
                              long hz)
{
    if (prev->id.pid != curr->id.pid ||
        prev->id.starttime != curr->id.starttime)
        return -1.0f;

    unsigned long long delta_cpu =
        (curr->utime + curr->stime) - (prev->utime + prev->stime);

    double elapsed_seconds =
        (double)(curr->timestamp_ms - prev->timestamp_ms) / 1000.0;

    if (elapsed_seconds <= 0.0)
        return 0.0f;

    float cpu_pct = (float)(100.0 * (double)delta_cpu /
                            (elapsed_seconds * (double)hz));

    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    float max_pct = 100.0f * (float)(ncpus > 0 ? ncpus : 1);
    if (cpu_pct < 0.0f)    cpu_pct = 0.0f;
    if (cpu_pct > max_pct) cpu_pct = max_pct;
    return cpu_pct;
}
```

### 8. `tests/unit/test_metrics.c`

```
TEST(idle_process)
  delta_cpu=0, elapsed_ms=1000, hz=100 → 0.0f

TEST(one_percent)
  delta_cpu=1, elapsed_ms=1000, hz=100 → 1.0f
  (1 tick / (1.0s * 100 ticks/s) * 100 = 1%)

TEST(recycled_pid)
  prev.id.starttime != curr.id.starttime → -1.0f

TEST(full_cpu_saturates_clamp)
  ncpus = sysconf(_SC_NPROCESSORS_ONLN)
  delta_cpu = (unsigned long long)(1.0 * hz * ncpus)  /* satura exactamente el clamp */
  elapsed_ms = 1000, hz = 100
  resultado <= 100.0f * ncpus  &&  resultado >= 99.0f * ncpus
  (no comparar con valor fijo: el clamp depende de num_cpus del host)

TEST(zero_elapsed)
  elapsed_ms=0 → 0.0f (no div-by-zero)
```

Agregar al Makefile:
```makefile
$(BUILD_DIR)/test_metrics: $(TEST_UNIT_DIR)/test_metrics.c \
		src/metrics/metrics.c $(BUILD_DIR)/unity.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CFLAGS_ASAN) \
	  -I$(UNITY_DIR) -Isrc/common -Isrc/metrics \
	  $^ -o $@ $(LDFLAGS) -fsanitize=address -fsanitize=undefined

TEST_BINS := $(BUILD_DIR)/test_collector $(BUILD_DIR)/test_metrics
```

---

## Sesión 3 — main.c de integración + output visible

> **Nota post-implementación (2026-04-19)**: el sketch de §9 abajo se
> mantiene como referencia histórica. La revisión crítica pre-TDD detectó
> tres defectos (firma de 3 args de `pg_metrics_cpu_percent` en lugar de 4,
> `cmp_cpu_desc` referenciando un typedef local a main, y retorno de
> `pg_collector_scan` ignorado) y tres huecos de decisión. La versión
> shipped vive en `src/main.c` y sus decisiones están registradas como
> **ADR-014** (filtrar sentinel -1.0f en el ranking), **ADR-015**
> (fail-loud en errores de scan) y **ADR-016** (extracción de
> `src/util/rank` + unit tests del comparador). Lee los ADRs y el código,
> no el sketch de abajo, si quieres entender el integrador actual.

### 9. `src/main.c` (reemplaza el stub)

```c
/* Dos scans con sleep(1), calcula CPU%, imprime top-5. Integrador temporal
 * de Slice 1 — reemplazado en Slice 2 por main con hilos y config. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "collector.h"
#include "metrics.h"

/* comparador para qsort descendente por cpu_pct */
static int cmp_cpu_desc(const void *a, const void *b);

int main(int argc, char *argv[])
{
    const char *proc_base = (argc > 1) ? argv[1] : "/proc";
    long hz = sysconf(_SC_CLK_TCK);

    pg_collector_t *col = NULL;
    if (pg_collector_init(&col, proc_base) != PG_OK) {
        fprintf(stderr, "procguard: failed to init collector\n");
        return 1;
    }

    pg_raw_sample_t *prev = NULL; size_t prev_n = 0;
    pg_collector_scan(col, &prev, &prev_n);
    sleep(1);
    pg_raw_sample_t *curr = NULL; size_t curr_n = 0;
    pg_collector_scan(col, &curr, &curr_n);

    /* struct local para ordenar */
    typedef struct { int pid; char comm[256]; float cpu; } ranked_t;
    ranked_t *ranked = malloc(curr_n * sizeof(ranked_t));
    if (!ranked) { free(prev); free(curr); pg_collector_destroy(col); return 1; }

    size_t n = 0;
    for (size_t i = 0; i < curr_n; i++) {
        for (size_t j = 0; j < prev_n; j++) {
            if (curr[i].id.pid == prev[j].id.pid &&
                curr[i].id.starttime == prev[j].id.starttime) {
                ranked[n].pid = (int)curr[i].id.pid;
                ranked[n].cpu = pg_metrics_cpu_percent(&prev[j], &curr[i], hz);
                /* snprintf seguro: comm ya está null-terminated por el collector */
                snprintf(ranked[n].comm, sizeof(ranked[n].comm),
                         "%s", curr[i].comm);
                n++;
                break;
            }
        }
    }

    qsort(ranked, n, sizeof(ranked_t), cmp_cpu_desc);

    printf("%-8s %-20s %7s\n", "PID", "COMMAND", "CPU%");
    size_t top = n < 5 ? n : 5;
    for (size_t i = 0; i < top; i++)
        printf("%-8d %-20s %6.1f%%\n", ranked[i].pid, ranked[i].comm, ranked[i].cpu);

    free(ranked); free(prev); free(curr);
    pg_collector_destroy(col);
    return 0;
}

static int cmp_cpu_desc(const void *a, const void *b)
{
    float fa = ((const ranked_t *)a)->cpu;
    float fb = ((const ranked_t *)b)->cpu;
    return (fb > fa) - (fb < fa);
}
```

Actualizar `$(BUILD_DIR)/procguard` para incluir metrics.c:
```makefile
$(BUILD_DIR)/procguard: src/collector/collector.c \
		src/metrics/metrics.c src/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Isrc/common -Isrc/collector -Isrc/metrics \
	  $^ -o $@ $(LDFLAGS) $(LDLIBS)
```

### 10. Verificación final de Slice 1

```bash
make asan                                    # exit 0, cero warnings
make test                                    # 0 failures en Unity output
make valgrind                                # 0 bytes lost
./build/procguard                            # tabla de procesos por CPU%
./build/procguard /tmp/pg_test_proc          # corre contra procfs sintético
```

---

## Criterio de completitud

- `make asan`: exit 0, cero warnings (-Wall -Wextra -Werror)
- `make test`: Unity reporta 0 failures en todos los test binaries
- `make valgrind`: exit 0, 0 bytes in 0 blocks lost
- `./build/procguard`: output visible con procesos reales y %CPU
- Commit verde: `"add slice-1: collector, metrics cpu-percent, integration main"`
- `docs/STATE.md` actualizado con los módulos completados
