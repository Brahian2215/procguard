#include "collector.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PG_STAT_BUF_SZ      4096
#define PG_PID_INITIAL_CAP  64
#define PG_PATH_MAX         512
#define PG_NS_PER_MS        1000000ULL
#define PG_MS_PER_SEC       1000ULL

struct pg_collector {
    char *proc_base;
    bool  skip_kt; /* si true, scan omite kthreadd y sus hijos (ADR-021) */
};

/* --- helpers internos ---------------------------------------------------- */

static int is_all_digits(const char *s)
{
    if (s == NULL || *s == '\0') {
        return 0;
    }
    for (const char *p = s; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
    }
    return 1;
}

static unsigned long long monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (unsigned long long)ts.tv_sec * PG_MS_PER_SEC +
           (unsigned long long)ts.tv_nsec / PG_NS_PER_MS;
}

/* Lee el contenido de path en buf (tamaño bufsz). Retorna PG_OK o PG_ERR_IO. */
static int read_file(const char *path, char *buf, size_t bufsz)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return PG_ERR_IO;
    }
    size_t n = fread(buf, 1, bufsz - 1, f);
    fclose(f);
    if (n == 0) {
        return PG_ERR_IO;
    }
    buf[n] = '\0';
    return PG_OK;
}

/* Parsea los campos posicionales post-comm de /proc/[pid]/stat.
 * Posiciones absolutas (proc(5)): state=3, ppid=4, tty_nr=7,
 * utime=14, stime=15, starttime=22. %*s descarta tokens
 * (mezclar '*' con length modifier es incompatible en gnu_scanf). */
static int parse_stat_fields(const char *post_comm, pg_raw_sample_t *s)
{
    int matched = sscanf(post_comm,
        " %c %ld %*s %*s %d %*s %*s "
        "%*s %*s %*s %*s "
        "%llu %llu %*s %*s %*s %*s %*s %*s %llu",
        &s->state, &s->ppid, &s->tty_nr,
        &s->utime, &s->stime, &s->id.starttime);
    return (matched == 6) ? PG_OK : PG_ERR_PARSE;
}

/* Parsea una línea de /proc/[pid]/stat en sample. El campo comm está
 * delimitado por el primer '(' y el ÚLTIMO ')' (el nombre puede contener
 * espacios y paréntesis internos). Retorna PG_OK o PG_ERR_PARSE. */
static int parse_stat(const char *line, pg_raw_sample_t *sample)
{
    const char *open_paren = strchr(line, '(');
    const char *close_paren = strrchr(line, ')');
    if (open_paren == NULL || close_paren == NULL || close_paren <= open_paren) {
        return PG_ERR_PARSE;
    }

    long pid = 0;
    if (sscanf(line, "%ld", &pid) != 1) {
        return PG_ERR_PARSE;
    }

    size_t comm_len = (size_t)(close_paren - open_paren - 1);
    if (comm_len >= sizeof(sample->comm)) {
        comm_len = sizeof(sample->comm) - 1;
    }
    memcpy(sample->comm, open_paren + 1, comm_len);
    sample->comm[comm_len] = '\0';

    if (parse_stat_fields(close_paren + 2, sample) != PG_OK) {
        return PG_ERR_PARSE;
    }
    sample->id.pid = (pid_t)pid;
    return PG_OK;
}

/* Lee y parsea /proc/[pid]/stat. */
static int read_proc_stat(const char *proc_base, const char *pid_str,
                          pg_raw_sample_t *sample)
{
    char path[PG_PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/%s/stat",
                           proc_base, pid_str);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return PG_ERR_PARSE;
    }

    char buf[PG_STAT_BUF_SZ];
    if (read_file(path, buf, sizeof(buf)) != PG_OK) {
        return PG_ERR_IO;
    }
    return parse_stat(buf, sample);
}

/* Lee /proc/[pid]/statm y popula sample->vmrss_bytes.
 * Formato: "size resident shared text lib data dt" (páginas).
 * Best-effort: si falla, deja vmrss_bytes intacto (caller lo inicializó a 0).
 * %*s salta el primer campo sin gatillar -Werror=format= (ver
 * MAKEFILE_GOTCHAS §1; no se puede usar %*lu/%*llu con supresor). */
static int read_proc_statm(const char *proc_base, const char *pid_str,
                           pg_raw_sample_t *sample)
{
    char path[PG_PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/%s/statm",
                           proc_base, pid_str);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return PG_ERR_PARSE;
    }

    char buf[PG_STAT_BUF_SZ];
    if (read_file(path, buf, sizeof(buf)) != PG_OK) {
        return PG_ERR_IO;
    }

    unsigned long long resident_pages = 0;
    if (sscanf(buf, "%*s %llu", &resident_pages) != 1) {
        return PG_ERR_PARSE;
    }
    sample->vmrss_bytes =
        resident_pages * (unsigned long long)sysconf(_SC_PAGESIZE);
    return PG_OK;
}

/* Crece arr a new_cap elementos. Retorna PG_OK o PG_ERR_MEM. */
static int grow_array(pg_raw_sample_t **arr, size_t new_cap)
{
    pg_raw_sample_t *tmp = realloc(*arr, new_cap * sizeof(**arr));
    if (tmp == NULL) {
        return PG_ERR_MEM;
    }
    *arr = tmp;
    return PG_OK;
}

/* --- API pública --------------------------------------------------------- */

int pg_collector_init(pg_collector_t **col, const char *proc_base,
                      bool skip_kernel_threads)
{
    if (col == NULL || proc_base == NULL) {
        return PG_ERR_PARSE;
    }

    pg_collector_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return PG_ERR_MEM;
    }

    c->proc_base = strdup(proc_base);
    if (c->proc_base == NULL) {
        free(c);
        return PG_ERR_MEM;
    }
    c->skip_kt = skip_kernel_threads;

    *col = c;
    return PG_OK;
}

int pg_collector_scan(pg_collector_t *col,
                      pg_raw_sample_t **out, size_t *out_count)
{
    if (col == NULL || out == NULL || out_count == NULL) {
        return PG_ERR_PARSE;
    }

    DIR *dir = opendir(col->proc_base);
    if (dir == NULL) {
        return PG_ERR_IO;
    }

    /* D1: una sola medición de timestamp para todas las muestras del scan */
    unsigned long long ts_ms = monotonic_ms();

    pg_raw_sample_t *arr = NULL;
    size_t n = 0;
    size_t cap = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!is_all_digits(ent->d_name)) {
            continue;
        }
        if (n == cap) {
            size_t new_cap = (cap == 0) ? PG_PID_INITIAL_CAP : cap * 2;
            if (grow_array(&arr, new_cap) != PG_OK) {
                free(arr);
                closedir(dir);
                return PG_ERR_MEM;
            }
            cap = new_cap;
        }
        pg_raw_sample_t sample;
        memset(&sample, 0, sizeof(sample));
        if (read_proc_stat(col->proc_base, ent->d_name, &sample) != PG_OK) {
            continue; /* skip silencioso (best-effort) */
        }
        /* statm y io son best-effort aditivos: si fallan, los campos
         * quedan en 0 y el sample se incluye igualmente (ADR-022). */
        (void)read_proc_statm(col->proc_base, ent->d_name, &sample);
        sample.timestamp_ms = ts_ms;
        arr[n++] = sample;
    }
    closedir(dir);

    *out = arr;
    *out_count = n;
    return PG_OK;
}

void pg_collector_destroy(pg_collector_t *col)
{
    if (col == NULL) {
        return;
    }
    free(col->proc_base);
    free(col);
}
