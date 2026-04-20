#include "metrics.h"

#include <stddef.h>

float pg_metrics_cpu_percent(const pg_raw_sample_t *prev,
                             const pg_raw_sample_t *curr,
                             long hz,
                             long ncpus)
{
    if (prev == NULL || curr == NULL) {
        return -1.0f;
    }
    if (prev->id.pid != curr->id.pid ||
        prev->id.starttime != curr->id.starttime) {
        return -1.0f;
    }

    unsigned long long prev_cpu = prev->utime + prev->stime;
    unsigned long long curr_cpu = curr->utime + curr->stime;
    if (curr_cpu < prev_cpu) {
        return -1.0f; /* violación de monotonía de jiffies — ADR-012 */
    }

    double elapsed_s =
        (double)(curr->timestamp_ms - prev->timestamp_ms) / 1000.0;
    if (elapsed_s <= 0.0) {
        return 0.0f;
    }

    unsigned long long delta_cpu = curr_cpu - prev_cpu;
    float cpu_pct = (float)(100.0 * (double)delta_cpu /
                            (elapsed_s * (double)hz));

    long n = ncpus > 0 ? ncpus : 1;
    float max_pct = 100.0f * (float)n;
    if (cpu_pct < 0.0f)    cpu_pct = 0.0f;
    if (cpu_pct > max_pct) cpu_pct = max_pct;
    return cpu_pct;
}
