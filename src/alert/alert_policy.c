#include "alert_policy.h"

/* Stub RED: implementación en GREEN (paso 3). Mantiene signatura para
 * que tests linkeen; todo return PG_ERR_PARSE fuerza los asserts a fallar. */

int pg_policy_catalog_load(const char           *ini_path,
                           pg_policy_t         **out_policies,
                           size_t               *out_n_policies,
                           pg_global_config_t   *out_global,
                           pg_security_config_t *out_security,
                           size_t               *out_n_errors)
{
    (void)ini_path;
    (void)out_policies;
    (void)out_n_policies;
    (void)out_global;
    (void)out_security;
    (void)out_n_errors;
    return PG_ERR_PARSE;
}

void pg_policy_catalog_destroy(pg_policy_t *policies, size_t n)
{
    (void)policies;
    (void)n;
}

void pg_security_config_destroy(pg_security_config_t *sec)
{
    (void)sec;
}
