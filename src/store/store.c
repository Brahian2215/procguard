#include "store.h"

#include <stdbool.h>
#include <stdlib.h>

typedef struct {
    pg_proc_id_t       id;
    pg_raw_sample_t   *samples;        /* array circular de n_per_proc slots */
    size_t             head;           /* próximo slot de escritura */
    size_t             count;          /* slots ocupados (<= n_per_proc) */
    unsigned int       absent_cycles;  /* ticks consecutivos sin insert */
    bool               seen_this_tick; /* set en insert, reset en tick */
} pg_store_entry_t;

struct pg_store {
    size_t              n_per_proc;
    pg_store_entry_t   *entries;
    size_t              n_entries;
    size_t              cap;
};

/* Devuelve el índice de la entry con ese id, o store->n_entries si no existe
 * (sentinel "no encontrado" análogo a std::string::npos). */
static size_t find_entry_idx(const pg_store_t *store, pg_proc_id_t id)
{
    for (size_t i = 0; i < store->n_entries; i++) {
        if (store->entries[i].id.pid == id.pid &&
            store->entries[i].id.starttime == id.starttime) {
            return i;
        }
    }
    return store->n_entries;
}

/* Amortiza realloc: cap=0 → 4, luego doblamos. Retorna PG_ERR_MEM si falla. */
static int ensure_capacity(pg_store_t *store)
{
    if (store->n_entries < store->cap) {
        return PG_OK;
    }
    size_t new_cap = (store->cap == 0) ? 4 : store->cap * 2;
    pg_store_entry_t *tmp = realloc(store->entries, new_cap * sizeof(*tmp));
    if (tmp == NULL) {
        return PG_ERR_MEM;
    }
    store->entries = tmp;
    store->cap = new_cap;
    return PG_OK;
}

/* Crea una entry nueva para id y la inicializa; retorna puntero vía *out. */
static int create_entry(pg_store_t *store, pg_proc_id_t id,
                        pg_store_entry_t **out)
{
    int rc = ensure_capacity(store);
    if (rc != PG_OK) {
        return rc;
    }
    pg_raw_sample_t *samples = calloc(store->n_per_proc, sizeof(*samples));
    if (samples == NULL) {
        return PG_ERR_MEM;
    }
    pg_store_entry_t *e = &store->entries[store->n_entries++];
    e->id = id;
    e->samples = samples;
    e->head = 0;
    e->count = 0;
    e->absent_cycles = 0;
    e->seen_this_tick = false;
    *out = e;
    return PG_OK;
}

int pg_store_init(pg_store_t **store, size_t n_per_proc)
{
    if (store == NULL || n_per_proc == 0) {
        return PG_ERR_PARSE;
    }
    pg_store_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return PG_ERR_MEM;
    }
    s->n_per_proc = n_per_proc;
    *store = s;
    return PG_OK;
}

int pg_store_insert(pg_store_t *store, const pg_raw_sample_t *sample)
{
    if (store == NULL || sample == NULL) {
        return PG_ERR_PARSE;
    }
    size_t idx = find_entry_idx(store, sample->id);
    pg_store_entry_t *e;
    bool was_existing;
    if (idx == store->n_entries) {
        int rc = create_entry(store, sample->id, &e);
        if (rc != PG_OK) {
            return rc;
        }
        was_existing = false;
    } else {
        e = &store->entries[idx];
        was_existing = true;
    }

    e->samples[e->head] = *sample;
    e->head = (e->head + 1) % store->n_per_proc;
    if (e->count < store->n_per_proc) {
        e->count++;
    }
    /* Sólo re-inserts marcan seen_this_tick: el próximo tick resetea absent.
     * Entries recién creadas arrancan con absent=0 y seen=false; el próximo
     * tick incrementa absent a 1 — así "sobrevive grace_cycles ticks sin
     * reinserts" cuenta desde el tick posterior al insert, no desde uno más.
     */
    if (was_existing) {
        e->seen_this_tick = true;
    }
    return PG_OK;
}

int pg_store_get_history(const pg_store_t *store,
                         pg_proc_id_t id,
                         pg_raw_sample_t *buf, size_t buf_cap,
                         size_t *out_len)
{
    if (store == NULL || buf == NULL || buf_cap == 0 || out_len == NULL) {
        return PG_ERR_PARSE;
    }
    size_t idx = find_entry_idx(store, id);
    if (idx == store->n_entries) {
        *out_len = 0;
        return PG_OK;
    }
    const pg_store_entry_t *e = &store->entries[idx];
    size_t n = store->n_per_proc;
    size_t take = (buf_cap < e->count) ? buf_cap : e->count;

    /* first_idx = oldest de los 'take' más recientes = head - take (mod n). */
    size_t first_idx = (e->head + n - take) % n;
    for (size_t i = 0; i < take; i++) {
        buf[i] = e->samples[(first_idx + i) % n];
    }
    *out_len = take;
    return PG_OK;
}

/* Libera los samples de la entry en idx y compacta por swap-con-último. */
static void free_entry_at(pg_store_t *store, size_t idx)
{
    free(store->entries[idx].samples);
    size_t last = store->n_entries - 1;
    if (idx != last) {
        store->entries[idx] = store->entries[last];
    }
    store->n_entries--;
}

int pg_store_tick(pg_store_t *store, unsigned int grace_cycles)
{
    if (store == NULL) {
        return PG_ERR_PARSE;
    }
    size_t i = 0;
    while (i < store->n_entries) {
        pg_store_entry_t *e = &store->entries[i];
        if (e->seen_this_tick) {
            e->seen_this_tick = false;
            e->absent_cycles = 0;
            i++;
            continue;
        }
        e->absent_cycles++;
        if (e->absent_cycles > grace_cycles) {
            /* Tras free_entry_at, el slot i contiene la ex-última entry;
             * no incrementar i para procesarla en la siguiente iteración. */
            free_entry_at(store, i);
            continue;
        }
        i++;
    }
    return PG_OK;
}

void pg_store_destroy(pg_store_t *store)
{
    if (store == NULL) {
        return;
    }
    for (size_t i = 0; i < store->n_entries; i++) {
        free(store->entries[i].samples);
    }
    free(store->entries);
    free(store);
}
