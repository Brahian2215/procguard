#ifndef PG_RANK_H
#define PG_RANK_H

#include "pg_types.h"

/*
 * ranked_t — fila materializada para el ranking top-N por CPU%.
 * comm[PG_COMM_MAX] enlaza explícitamente con pg_raw_sample_t::comm;
 * se copia con memcpy para evitar -Werror=format-truncation.
 */
typedef struct {
    pid_t pid;
    char  comm[PG_COMM_MAX];
    float cpu;
} ranked_t;

/*
 * pg_rank_cmp_cpu_desc — comparador qsort que ordena ranked_t[]
 * descendente por .cpu. Se asume que los sentinel (-1.0f) han sido
 * filtrados por el caller antes del sort (ADR-014); aquí no se aplica
 * lógica especial para valores negativos.
 */
int pg_rank_cmp_cpu_desc(const void *a, const void *b);

#endif /* PG_RANK_H */
