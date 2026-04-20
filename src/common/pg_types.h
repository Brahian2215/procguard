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
} pg_raw_sample_t;

/* vmrss se introduce en Slice 2 (requiere leer /proc/[pid]/statm por separado) */

#define PG_GRACE_CYCLES 10

#define PG_OK         0
#define PG_ERR_IO    -1
#define PG_ERR_PARSE -2
#define PG_ERR_MEM   -3

#endif /* PG_TYPES_H */
