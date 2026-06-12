/*
 * Orquestador de gobernanza (Slice 4b Fase 7). Reemplaza el integrador
 * temporal de Slice 1/2. Loop secuencial de 10 ciclos:
 *   scan → insert → engine_cycle → store_tick → engine_gc   (ADR-013)
 * Stdout: top-5 por CPU% de cada ciclo. Las decisiones del engine se loguean
 * a stderr desde act() (formato [alert] ...). dry_run por defecto (config).
 * El modelo con hilos (gobernanza/inotify/TUI) llega en Slice 5.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "alert.h"
#include "collector.h"
#include "metrics.h"
#include "store.h"

#define PG_CYCLES 10

typedef struct {
    const char *config;
    const char *proc_base;
    int         cycles;
} pg_args_t;

typedef struct {
    pid_t pid;
    char  comm[PG_COMM_MAX];
    float cpu;
} ranked_t;

static void parse_args(int argc, char **argv, pg_args_t *a)
{
    a->config    = "config/procguard.ini";
    a->proc_base = "/proc";
    a->cycles    = PG_CYCLES;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            a->config = argv[++i];
        } else if (strcmp(argv[i], "--proc") == 0 && i + 1 < argc) {
            a->proc_base = argv[++i];
        } else if (strcmp(argv[i], "--cycles") == 0 && i + 1 < argc) {
            int n = atoi(argv[++i]);
            if (n > 0) {
                a->cycles = n;
            }
        }
    }
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

static unsigned long long now_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)ts.tv_nsec / 1000000ULL;
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000u, (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
}

static int cmp_cpu_desc(const void *a, const void *b)
{
    float fa = ((const ranked_t *)a)->cpu;
    float fb = ((const ranked_t *)b)->cpu;
    return (fb > fa) - (fb < fa);
}

/* Empareja curr↔prev por (pid, starttime) y calcula CPU% para el top. */
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

static void report_top(const pg_raw_sample_t *prev, size_t prev_n,
                       const pg_raw_sample_t *curr, size_t curr_n,
                       long hz, long ncpus, size_t top_k)
{
    if (curr_n == 0) {
        print_top(NULL, 0, top_k);
        return;
    }
    ranked_t *ranked = malloc(curr_n * sizeof(*ranked));
    if (ranked == NULL) {
        return;   /* best-effort: el ciclo sigue sin top */
    }
    size_t n = build_ranked(prev, prev_n, curr, curr_n, hz, ncpus, ranked);
    qsort(ranked, n, sizeof(*ranked), cmp_cpu_desc);
    print_top(ranked, n, top_k);
    free(ranked);
}

static void insert_all(pg_store_t *s, const pg_raw_sample_t *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        pg_store_insert(s, &v[i]);   /* best-effort: drop si el store falla */
    }
}

/* Un ciclo de gobernanza completo (ADR-013). Retorna 0 / 1 (fallo de scan). */
static int run_cycle(pg_collector_t *col, pg_alert_engine_t *eng,
                     pg_store_t *store, long hz, long ncpus,
                     pg_raw_sample_t **prev, size_t *prev_n, int idx)
{
    unsigned long long now = now_monotonic_ms();
    pg_raw_sample_t *curr = NULL;
    size_t curr_n = 0;
    if (pg_collector_scan(col, &curr, &curr_n) != PG_OK) {
        fprintf(stderr, "procguard: scan failed (ciclo %d)\n", idx + 1);
        return 1;
    }
    insert_all(store, curr, curr_n);
    pg_alert_engine_cycle(eng, curr, curr_n, store, now);   /* act → stderr */
    printf("\n=== ciclo %d (now=%llu ms, %zu procesos) ===\n",
           idx + 1, now, curr_n);
    report_top(*prev, *prev_n, curr, curr_n, hz, ncpus, 5);
    pg_store_tick(store, PG_GRACE_CYCLES);
    pg_alert_engine_gc(eng, store);
    free(*prev);
    *prev = curr;
    *prev_n = curr_n;
    return 0;
}

static int run_loop(pg_collector_t *col, pg_alert_engine_t *eng,
                    pg_store_t *store, long hz, long ncpus,
                    unsigned interval, int cycles)
{
    pg_raw_sample_t *prev = NULL;
    size_t prev_n = 0;
    int rc = 0;
    for (int c = 0; c < cycles; c++) {
        if (run_cycle(col, eng, store, hz, ncpus, &prev, &prev_n, c) != 0) {
            rc = 1;
            break;
        }
        if (c < cycles - 1) {
            sleep_ms(interval);
        }
    }
    free(prev);
    return rc;
}

int main(int argc, char **argv)
{
    pg_args_t args;
    parse_args(argc, argv, &args);
    long hz, ncpus;
    if (read_system_params(&hz, &ncpus) != 0) {
        return 1;
    }
    pg_collector_t *col = NULL;
    if (pg_collector_init(&col, args.proc_base, false) != PG_OK) {
        fprintf(stderr, "procguard: collector init failed\n");
        return 1;
    }
    pg_alert_engine_t *eng = NULL;
    int rc = pg_alert_engine_init(&eng, args.config, args.proc_base,
                                  getpid(), hz, ncpus, NULL);
    if (rc != PG_OK) {
        fprintf(stderr, "procguard: engine init failed (rc=%d) cargando %s\n",
                rc, args.config);
        pg_collector_destroy(col);
        return 1;
    }
    const pg_global_config_t *g = pg_alert_engine_global(eng);
    pg_store_t *store = NULL;
    if (pg_store_init(&store, g->sample_buffer) != PG_OK) {
        fprintf(stderr, "procguard: store init failed\n");
        pg_alert_engine_destroy(eng);
        pg_collector_destroy(col);
        return 1;
    }
    printf("procguard: config=%s proc=%s dry_run=%s interval=%u ms x %d ciclos\n",
           args.config, args.proc_base, g->dry_run ? "true" : "false",
           g->sample_interval_ms, args.cycles);
    rc = run_loop(col, eng, store, hz, ncpus, g->sample_interval_ms, args.cycles);

    pg_store_destroy(store);
    pg_alert_engine_destroy(eng);
    pg_collector_destroy(col);
    return rc;
}
