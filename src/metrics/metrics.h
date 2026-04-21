#ifndef PG_METRICS_H
#define PG_METRICS_H

#include "pg_types.h"

/*
 * pg_metrics_cpu_percent — porcentaje de CPU usado entre dos muestras
 * del mismo proceso, calculado sobre los jiffies utime+stime y el
 * intervalo temporal en timestamp_ms.
 *
 * hz     jiffies por segundo del host (sysconf(_SC_CLK_TCK)).
 * ncpus  número de CPUs en línea (sysconf(_SC_NPROCESSORS_ONLN));
 *        define el clamp superior del resultado.
 *
 * Ambos parámetros se inyectan explícitamente (ADR-008) para hacer
 * la función determinista y testable sin depender del host.
 *
 * Retorna:
 *   -1.0f  muestra inutilizable (ADR-006):
 *          - prev o curr es NULL
 *          - prev->id.pid != curr->id.pid
 *          - prev->id.starttime != curr->id.starttime
 *          - (curr->utime + curr->stime) < (prev->utime + prev->stime)
 *            (underflow; guarda defensiva ante posibles violaciones de
 *            monotonía de jiffies para un mismo pid+starttime)
 *    0.0f  elapsed_ms == 0 (evita división por cero)
 *   [0.0f, 100.0f * max(ncpus, 1)]  CPU% clampeado en caso normal
 */
float pg_metrics_cpu_percent(const pg_raw_sample_t *prev,
                             const pg_raw_sample_t *curr,
                             long hz,
                             long ncpus);

/*
 * pg_io_rates_t — tasas I/O por segundo calculadas entre dos muestras.
 * Counters subyacentes en bytes (read_bytes, write_bytes) o en bytes lógicos
 * desde el POV del proceso (rchar, wchar, incluye cache hits).
 */
typedef struct {
    float rchar_per_s;
    float wchar_per_s;
    float read_bytes_per_s;
    float write_bytes_per_s;
} pg_io_rates_t;

/*
 * pg_metrics_io_rates — llena *out con las 4 tasas I/O entre prev y curr.
 *
 * Semántica de sentinels (espejo de pg_metrics_cpu_percent, ADR-006):
 *   out == NULL                          → no-op.
 *   prev/curr NULL o id mismatch         → los 4 campos a -1.0f.
 *   elapsed_ms == 0                      → los 4 campos a 0.0f (evita div/0).
 *   underflow en un counter aislado      → sólo ese campo a -1.0f; los
 *                                          otros tres conservan su valor
 *                                          (los counters son independientes).
 *
 * No hay clamp superior: las tasas I/O no tienen techo teórico.
 */
void pg_metrics_io_rates(const pg_raw_sample_t *prev,
                         const pg_raw_sample_t *curr,
                         pg_io_rates_t *out);

/*
 * RSS: no hay función dedicada. vmrss_bytes de pg_raw_sample_t ya está en
 * bytes y es un pase-directo del campo 2 de /proc/[pid]/statm * pagesize.
 * Consumidores de M3 leen sample->vmrss_bytes sin intermediación.
 */

#endif /* PG_METRICS_H */
