#include "alert_state.h"

#include "store.h"

#include <stdbool.h>
#include <stdlib.h>

struct pg_alert_state_registry {
    pg_alert_state_t **states;   /* punteros estables; cada *states[i] es heap */
    size_t             n_entries;
    size_t             cap;
};

/* Devuelve el índice de la entry con esa identidad, o reg->n_entries si no
 * existe (sentinel "no encontrado"). Análogo a find_entry_idx del store. */
static size_t find_idx(const pg_alert_state_registry_t *reg,
                       pg_proc_id_t id, size_t policy_index)
{
    for (size_t i = 0; i < reg->n_entries; i++) {
        const pg_alert_state_t *s = reg->states[i];
        if (s->id.pid == id.pid &&
            s->id.starttime == id.starttime &&
            s->policy_index == policy_index) {
            return i;
        }
    }
    return reg->n_entries;
}

/* Amortiza realloc del array de punteros: 0 → 4, luego doblamos. Los
 * targets de los punteros NO se mueven; estabilidad por diseño. */
static int ensure_capacity(pg_alert_state_registry_t *reg)
{
    if (reg->n_entries < reg->cap) {
        return PG_OK;
    }
    size_t new_cap = (reg->cap == 0) ? 4 : reg->cap * 2;
    pg_alert_state_t **tmp = realloc(reg->states, new_cap * sizeof(*tmp));
    if (tmp == NULL) {
        return PG_ERR_MEM;
    }
    reg->states = tmp;
    reg->cap = new_cap;
    return PG_OK;
}

int pg_alert_state_registry_init(pg_alert_state_registry_t **reg)
{
    if (reg == NULL) {
        return PG_ERR_PARSE;
    }
    pg_alert_state_registry_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return PG_ERR_MEM;
    }
    *reg = r;
    return PG_OK;
}

int pg_alert_state_upsert(pg_alert_state_registry_t *reg,
                          pg_proc_id_t id, size_t policy_index,
                          pg_alert_state_t **out)
{
    if (reg == NULL || out == NULL) {
        return PG_ERR_PARSE;
    }
    size_t idx = find_idx(reg, id, policy_index);
    if (idx < reg->n_entries) {
        *out = reg->states[idx];
        return PG_OK;
    }
    int rc = ensure_capacity(reg);
    if (rc != PG_OK) {
        return rc;
    }
    pg_alert_state_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return PG_ERR_MEM;
    }
    s->id = id;
    s->policy_index = policy_index;
    reg->states[reg->n_entries++] = s;
    *out = s;
    return PG_OK;
}

pg_alert_state_t *pg_alert_state_lookup(pg_alert_state_registry_t *reg,
                                        pg_proc_id_t id,
                                        size_t policy_index)
{
    if (reg == NULL) {
        return NULL;
    }
    size_t idx = find_idx(reg, id, policy_index);
    if (idx == reg->n_entries) {
        return NULL;
    }
    return reg->states[idx];
}

/* Aprovecha el contrato de pg_store_get_history: out_len==0 si el id no
 * existe en el store (ya liberado por pg_store_tick). Buffer de 1 sample
 * en stack — la copia es trivial frente al costo del recorrido lineal del
 * store (find_entry_idx). */
static bool id_in_store(const pg_store_t *store, pg_proc_id_t id)
{
    pg_raw_sample_t buf;
    size_t out_len = 0;
    if (pg_store_get_history(store, id, &buf, 1, &out_len) != PG_OK) {
        return false;
    }
    return out_len > 0;
}

void pg_alert_state_gc(pg_alert_state_registry_t *reg,
                       const pg_store_t *store)
{
    if (reg == NULL || store == NULL) {
        return;
    }
    size_t i = 0;
    while (i < reg->n_entries) {
        if (id_in_store(store, reg->states[i]->id)) {
            i++;
            continue;
        }
        free(reg->states[i]);
        size_t last = reg->n_entries - 1;
        if (i != last) {
            reg->states[i] = reg->states[last];
        }
        reg->n_entries--;
        /* No incrementar i: el slot ahora contiene la ex-última entry y
         * debe revisarse en la siguiente iteración. */
    }
}

void pg_alert_state_registry_destroy(pg_alert_state_registry_t *reg)
{
    if (reg == NULL) {
        return;
    }
    for (size_t i = 0; i < reg->n_entries; i++) {
        free(reg->states[i]);
    }
    free(reg->states);
    free(reg);
}
