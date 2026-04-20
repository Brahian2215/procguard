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
 * Ambos parámetros se inyectan explícitamente (ADR-010) para hacer
 * la función determinista y testable sin depender del host.
 *
 * Retorna:
 *   -1.0f  muestra inutilizable (ADR-011):
 *          - prev o curr es NULL
 *          - prev->id.pid != curr->id.pid
 *          - prev->id.starttime != curr->id.starttime
 *          - (curr->utime + curr->stime) < (prev->utime + prev->stime)
 *            (underflow; posible si el kernel violara la monotonía
 *            de jiffies para un mismo pid+starttime — ADR-012)
 *    0.0f  elapsed_ms == 0 (evita división por cero)
 *   [0.0f, 100.0f * max(ncpus, 1)]  CPU% clampeado en caso normal
 */
float pg_metrics_cpu_percent(const pg_raw_sample_t *prev,
                             const pg_raw_sample_t *curr,
                             long hz,
                             long ncpus);

#endif /* PG_METRICS_H */
