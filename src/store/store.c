#include "store.h"

int pg_store_init(pg_store_t **store, size_t n_per_proc)
{
    (void)store;
    (void)n_per_proc;
    return PG_ERR_PARSE;
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
    (void)store;
}
