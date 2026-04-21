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
        return -1.0f; /* guarda defensiva: violación de monotonía de jiffies */
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

static float compute_rate(unsigned long long prev_c,
                          unsigned long long curr_c,
                          double elapsed_s)
{
    if (curr_c < prev_c) {
        return -1.0f; /* guarda defensiva: counter I/O no-monotónico */
    }
    return (float)((double)(curr_c - prev_c) / elapsed_s);
}

static void fill_sentinel(pg_io_rates_t *out, float v)
{
    out->rchar_per_s       = v;
    out->wchar_per_s       = v;
    out->read_bytes_per_s  = v;
    out->write_bytes_per_s = v;
}

void pg_metrics_io_rates(const pg_raw_sample_t *prev,
                         const pg_raw_sample_t *curr,
                         pg_io_rates_t *out)
{
    if (out == NULL) {
        return;
    }
    if (prev == NULL || curr == NULL ||
        prev->id.pid != curr->id.pid ||
        prev->id.starttime != curr->id.starttime) {
        fill_sentinel(out, -1.0f);
        return;
    }

    double elapsed_s =
        (double)(curr->timestamp_ms - prev->timestamp_ms) / 1000.0;
    if (elapsed_s <= 0.0) {
        fill_sentinel(out, 0.0f);
        return;
    }

    out->rchar_per_s       = compute_rate(prev->rchar,       curr->rchar,       elapsed_s);
    out->wchar_per_s       = compute_rate(prev->wchar,       curr->wchar,       elapsed_s);
    out->read_bytes_per_s  = compute_rate(prev->read_bytes,  curr->read_bytes,  elapsed_s);
    out->write_bytes_per_s = compute_rate(prev->write_bytes, curr->write_bytes, elapsed_s);
}
