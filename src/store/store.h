#ifndef PG_STORE_H
#define PG_STORE_H

#include "pg_types.h"
#include <stddef.h>

typedef struct pg_store pg_store_t; /* opaco */

/*
 * Inicializa el store con capacidad n_per_proc muestras por proceso.
 * Retorna:
 *   PG_OK         éxito (*store contiene un store inicializado)
 *   PG_ERR_PARSE  store == NULL o n_per_proc == 0
 *   PG_ERR_MEM    falló alocación interna
 */
int pg_store_init(pg_store_t **store, size_t n_per_proc);

/*
 * Inserta una muestra. Crea entry si el id no existe; empuja sobre el buffer
 * circular descartando la más antigua (FIFO) si lleno. Re-inserts marcan
 * la entry como "vista este tick" (pg_store_tick resetea absent_cycles);
 * entries nuevas no marcan ese flag — ya arrancan con absent=0 y no necesitan
 * reset en el próximo tick.
 *
 * Si el id fue liberado previamente por gracia vencida, un insert
 * posterior crea una entry nueva (count=0, sin histórico anterior).
 *
 * Invariante del caller: un mismo id no se inserta más de una vez entre
 * dos ticks consecutivos (el store no deduplica).
 *
 * Retorna:
 *   PG_OK         éxito
 *   PG_ERR_PARSE  store == NULL o sample == NULL
 *   PG_ERR_MEM    creación de entry falló
 */
int pg_store_insert(pg_store_t *store, const pg_raw_sample_t *sample);

/*
 * Copia hasta buf_cap muestras del histórico de id en buf (orden
 * cronológico: oldest first). *out_len recibe la cantidad real escrita
 * (<= buf_cap). Si buf_cap < count, se devuelven los buf_cap más
 * recientes, también en orden cronológico. Si el id no existe en el
 * store (nunca insertado o liberado por gracia), *out_len = 0 y retorna
 * PG_OK (ausencia no es error).
 *
 * Retorna:
 *   PG_OK         éxito
 *   PG_ERR_PARSE  store == NULL, buf == NULL, out_len == NULL o buf_cap == 0
 */
int pg_store_get_history(const pg_store_t *store,
                         pg_proc_id_t id,
                         pg_raw_sample_t *buf, size_t buf_cap,
                         size_t *out_len);

/*
 * Avanza un ciclo de gobernanza. Para cada entry:
 *  - si fue vista desde el último tick (insert ocurrió): resetea
 *    absent_cycles=0 y seen_this_tick=false.
 *  - si no fue vista: incrementa absent_cycles en 1; si excede
 *    grace_cycles libera la entry (swap-con-último, compactación).
 *
 * Semántica: entry insertada y nunca reinsertada sobrevive
 * grace_cycles ticks; se libera en el tick siguiente.
 *
 * Retorna:
 *   PG_OK         éxito
 *   PG_ERR_PARSE  store == NULL
 */
int pg_store_tick(pg_store_t *store, unsigned int grace_cycles);

/*
 * Libera el store y todos sus buffers. Seguro llamar con NULL (no-op).
 */
void pg_store_destroy(pg_store_t *store);

#endif /* PG_STORE_H */
