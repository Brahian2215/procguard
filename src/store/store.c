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

int pg_store_init(pg_store_t **store, size_t n_per_proc)
{
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
    (void)store;
    (void)sample;
    return PG_ERR_PARSE;
}

int pg_store_get_history(const pg_store_t *store,
                         pg_proc_id_t id,
                         pg_raw_sample_t *buf, size_t buf_cap,
                         size_t *out_len)
{
    (void)store;
    (void)id;
    (void)buf;
    (void)buf_cap;
    (void)out_len;
    return PG_ERR_PARSE;
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
