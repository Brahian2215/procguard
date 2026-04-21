/*
 * Integrador temporal de Slice 1/2. Dos scans con sleep(1), pareo por
 * (pid, starttime), top-5 por CPU% a stdout. Reemplazable por el ciclo
 * de gobernanza con hilos en Slice 4+.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "collector.h"
#include "metrics.h"

typedef struct {
    pid_t pid;
    char  comm[PG_COMM_MAX];
    float cpu;
} ranked_t;

static int cmp_cpu_desc(const void *a, const void *b)
{
    float fa = ((const ranked_t *)a)->cpu;
    float fb = ((const ranked_t *)b)->cpu;
    return (fb > fa) - (fb < fa);
}

static int read_system_params(long *hz, long *ncpus)
{
    *hz    = sysconf(_SC_CLK_TCK);
    *ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (*hz <= 0 || *ncpus <= 0) {
        fprintf(stderr, "procguard: sysconf failed (hz=%ld, ncpus=%ld)\n",
                *hz, *ncpus);
        return 1;
    }
    return 0;
}

static size_t build_ranked(const pg_raw_sample_t *prev, size_t prev_n,
                           const pg_raw_sample_t *curr, size_t curr_n,
                           long hz, long ncpus, ranked_t *out)
{
    size_t n = 0;
    for (size_t i = 0; i < curr_n; i++) {
        for (size_t j = 0; j < prev_n; j++) {
            if (curr[i].id.pid != prev[j].id.pid ||
                curr[i].id.starttime != prev[j].id.starttime) {
                continue;
            }
            float cpu = pg_metrics_cpu_percent(&prev[j], &curr[i], hz, ncpus);
            if (cpu < 0.0f) {
                break;
            }
            out[n].pid = curr[i].id.pid;
            out[n].cpu = cpu;
            memcpy(out[n].comm, curr[i].comm, sizeof(out[n].comm));
            n++;
            break;
        }
    }
    return n;
}

static void print_top(const ranked_t *r, size_t n, size_t top_k)
{
    printf("%-8s %-20s %7s\n", "PID", "COMMAND", "CPU%");
    size_t top = n < top_k ? n : top_k;
    for (size_t i = 0; i < top; i++) {
        printf("%-8d %-20s %6.1f%%\n", r[i].pid, r[i].comm, r[i].cpu);
    }
}

int main(int argc, char *argv[])
{
    const char *proc_base = (argc > 1) ? argv[1] : "/proc";
    long hz, ncpus;
    if (read_system_params(&hz, &ncpus) != 0) {
        return 1;
    }

    pg_collector_t *col = NULL;
    if (pg_collector_init(&col, proc_base, false) != PG_OK) {
        fprintf(stderr, "procguard: collector init failed\n");
        return 1;
    }

    pg_raw_sample_t *prev = NULL; size_t prev_n = 0;
    pg_raw_sample_t *curr = NULL; size_t curr_n = 0;
    ranked_t       *ranked = NULL;
    int rc = 0;

    if (pg_collector_scan(col, &prev, &prev_n) != PG_OK) {
        fprintf(stderr, "procguard: scan #1 failed\n");
        rc = 1; goto cleanup;
    }
    sleep(1);
    if (pg_collector_scan(col, &curr, &curr_n) != PG_OK) {
        fprintf(stderr, "procguard: scan #2 failed\n");
        rc = 1; goto cleanup;
    }

    if (curr_n > 0) {
        ranked = malloc(curr_n * sizeof(*ranked));
        if (ranked == NULL) {
            fprintf(stderr, "procguard: out of memory\n");
            rc = 1; goto cleanup;
        }
    }

    size_t n = build_ranked(prev, prev_n, curr, curr_n, hz, ncpus, ranked);
    qsort(ranked, n, sizeof(*ranked), cmp_cpu_desc);
    print_top(ranked, n, 5);

cleanup:
    free(ranked);
    free(prev);
    free(curr);
    pg_collector_destroy(col);
    return rc;
}
