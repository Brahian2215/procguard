#include "alert_state.h"

#include "store.h"

#include <stdlib.h>

struct pg_alert_state_registry {
    pg_alert_state_t **states;   /* punteros estables; cada *states[i] es heap */
    size_t             n_entries;
    size_t             cap;
};

int pg_alert_state_registry_init(pg_alert_state_registry_t **reg)
{
    (void)reg;
    return PG_ERR_PARSE;
}

int pg_alert_state_upsert(pg_alert_state_registry_t *reg,
                          pg_proc_id_t id, size_t policy_index,
                          pg_alert_state_t **out)
{
    (void)reg;
    (void)id;
    (void)policy_index;
    (void)out;
    return PG_ERR_PARSE;
}

pg_alert_state_t *pg_alert_state_lookup(pg_alert_state_registry_t *reg,
                                        pg_proc_id_t id,
                                        size_t policy_index)
{
    (void)reg;
    (void)id;
    (void)policy_index;
    return NULL;
}

void pg_alert_state_gc(pg_alert_state_registry_t *reg,
                       const pg_store_t *store)
{
    (void)reg;
    (void)store;
}

void pg_alert_state_registry_destroy(pg_alert_state_registry_t *reg)
{
    (void)reg;
}
