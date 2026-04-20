#ifndef PG_COLLECTOR_H
#define PG_COLLECTOR_H

#include "pg_types.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct pg_collector pg_collector_t; /* opaco */

/*
 * Inicializa el colector. proc_base es la raíz de procfs (normalmente "/proc").
 * skip_kernel_threads: si true, el scan omite procesos con pid == 2 (kthreadd)
 * o ppid == 2 (hilos de kernel). Heurística ADR-021.
 *
 * Retorna:
 *   PG_OK         éxito (*col contiene un colector inicializado)
 *   PG_ERR_PARSE  col == NULL o proc_base == NULL
 *   PG_ERR_MEM    falló alocación interna
 */
int pg_collector_init(pg_collector_t **col, const char *proc_base,
                      bool skip_kernel_threads);

/*
 * Escanea proc_base y retorna un array alocado con las muestras crudas.
 * Una sola medición de CLOCK_MONOTONIC se asigna como timestamp_ms a todas
 * las muestras del scan ("instante único" del scan).
 *
 * Ownership: *out debe ser liberado por el caller con free(*out).
 *
 * Retorna:
 *   PG_OK         éxito (*out, *out_count válidos)
 *   PG_ERR_PARSE  col, out u out_count == NULL
 *   PG_ERR_IO     proc_base inaccesible (opendir falló)
 *   PG_ERR_MEM    falló alocación del array de salida
 *
 * Errores por proceso individual (proceso desaparecido, stat malformado,
 * permisos denegados) son silenciosos: best-effort, se omite el proceso.
 */
int pg_collector_scan(pg_collector_t *col,
                      pg_raw_sample_t **out, size_t *out_count);

/*
 * Libera recursos del colector. Seguro llamar con col == NULL (no-op).
 */
void pg_collector_destroy(pg_collector_t *col);

#endif /* PG_COLLECTOR_H */
