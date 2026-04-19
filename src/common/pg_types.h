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

#define PG_OK         0
#define PG_ERR_IO    -1
#define PG_ERR_PARSE -2
#define PG_ERR_MEM   -3

#endif /* PG_TYPES_H */
