#ifndef PG_ALERT_STATE_H
#define PG_ALERT_STATE_H

#include "pg_types.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * M4 Alert & Governance — registry por (id, policy_index) → state.
 * Slice 4b Fase 3: estructura sin lógica de evaluación. Las Fases 4-6
 * (evaluate/validate/act) mutan los campos del state vía el puntero
 * devuelto por upsert.
 *
 * Usa forward decl de pg_store_t para no acoplar headers; la unidad de
 * compilación que implementa gc incluye store.h directamente.
 */
typedef struct pg_store pg_store_t;

/*
 * State por (proceso, política). Vive más allá de un solo ciclo: persiste
 * contadores de persistencia/histéresis, cooldown, escalamiento.
 *
 * - ADR-005: la identidad incluye `starttime`; pid reciclado con starttime
 *   distinto produce una entry independiente.
 * - ADR-013: durante ausencia (no hay sample en el ciclo), el state queda
 *   intacto — el freeze es responsabilidad del engine, no del registry.
 *   El registry solo libera entries cuando gc detecta que el id ya no
 *   está en el store (M2 ya aplicó la gracia G=10).
 */
typedef struct {
    pg_proc_id_t        id;
    size_t              policy_index;
    unsigned int        persistence;
    unsigned int        hysteresis;
    unsigned long long  cooldown_until_ms;
    unsigned long long  alert_active_since_ms;   /* PDF §7 cordura 5s */
    int                 escalation_level;
    bool                deactivated_since_last_act; /* avance vs reactivación */
} pg_alert_state_t;

typedef struct pg_alert_state_registry pg_alert_state_registry_t; /* opaco */

/*
 * Inicializa un registry vacío. *reg recibe el handle.
 * Retorna:
 *   PG_OK         éxito
 *   PG_ERR_PARSE  reg == NULL
 *   PG_ERR_MEM    OOM en alocación interna
 */
int pg_alert_state_registry_init(pg_alert_state_registry_t **reg);

/*
 * Devuelve vía *out un puntero al state de (id, policy_index). Crea la
 * entry zero-init si no existe.
 *
 * El puntero retornado es estable hasta que pg_alert_state_gc libere esa
 * entry. El registry almacena las entries vía punteros individuales,
 * de modo que el realloc del array índice no las invalida.
 *
 * Retorna:
 *   PG_OK         éxito (*out cargado)
 *   PG_ERR_PARSE  reg == NULL u out == NULL
 *   PG_ERR_MEM    OOM
 */
int pg_alert_state_upsert(pg_alert_state_registry_t *reg,
                          pg_proc_id_t id, size_t policy_index,
                          pg_alert_state_t **out);

/*
 * Devuelve puntero a la entry existente o NULL si no existe. No crea.
 * NULL-safe en reg.
 */
pg_alert_state_t *pg_alert_state_lookup(pg_alert_state_registry_t *reg,
                                        pg_proc_id_t id,
                                        size_t policy_index);

/*
 * Libera toda entry cuyo `id` ya no esté presente en el store. Debe
 * llamarse TRAS pg_store_tick (ADR-013): orden del integrador es
 * engine_cycle → store_tick → engine_gc.
 *
 * Detecta presencia vía pg_store_get_history(buf_cap=1): out_len == 0
 * ⇒ id ausente. NULL-safe en ambos argumentos.
 */
void pg_alert_state_gc(pg_alert_state_registry_t *reg,
                       const pg_store_t *store);

/*
 * Libera el registry y todas sus entries. NULL-safe (no-op, estilo free).
 */
void pg_alert_state_registry_destroy(pg_alert_state_registry_t *reg);

#endif /* PG_ALERT_STATE_H */
