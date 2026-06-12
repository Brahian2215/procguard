#ifndef PG_TYPES_H
#define PG_TYPES_H

#include <sys/types.h>

/*
 * Tamaño del buffer de nombre de proceso. 256 es holgura sobre el límite
 * real de Linux (TASK_COMM_LEN = 16) para permitir comm con paréntesis y
 * espacios sin truncar (parser en collector.c). Todo consumidor que copie
 * el comm debe dimensionarse con esta macro, no con el literal 256.
 */
#define PG_COMM_MAX 256

/*
 * Tamaño del buffer de la ruta real del ejecutable (readlink /proc/[pid]/exe).
 * 512 cubre rutas reales con holgura; rutas más largas se truncan (raro, y el
 * whitelist sólo necesita el prefijo). Kernel threads / procesos muertos →
 * exe_path[0]=='\0'. Consumidores usan esta macro, no el literal.
 */
#define PG_EXE_MAX 512

typedef struct {
    pid_t              pid;
    unsigned long long starttime;
} pg_proc_id_t;

typedef struct {
    pg_proc_id_t       id;
    char               comm[PG_COMM_MAX];
    char               state;
    long               ppid;
    int                tty_nr;
    unsigned long long utime;        /* jiffies en modo usuario */
    unsigned long long stime;        /* jiffies en modo kernel */
    unsigned long long timestamp_ms; /* CLOCK_MONOTONIC en ms al momento del scan */

    /* Campos append-only por convención (ver CLAUDE.md §convenciones). 0 si
     * la lectura del archivo correspondiente falla (permisos, race con exit,
     * kernel thread). */
    unsigned long long vmrss_bytes;  /* /proc/[pid]/statm campo 2 * pagesize */
    unsigned long long rchar;        /* /proc/[pid]/io */
    unsigned long long wchar;        /* /proc/[pid]/io */
    unsigned long long read_bytes;   /* /proc/[pid]/io */
    unsigned long long write_bytes;  /* /proc/[pid]/io */

    /* readlink(/proc/[pid]/exe). "" si kernel thread, proceso muerto o sin
     * permiso. Truncado a PG_EXE_MAX-1. Usado por M4 whitelist (ADR-012/016)
     * y M5 disguised_process. */
    char               exe_path[PG_EXE_MAX];
} pg_raw_sample_t;

#define PG_GRACE_CYCLES 10

#define PG_OK         0
#define PG_ERR_IO    -1
#define PG_ERR_PARSE -2
#define PG_ERR_MEM   -3

#endif /* PG_TYPES_H */
