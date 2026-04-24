#ifndef PG_ALERT_POLICY_H
#define PG_ALERT_POLICY_H

#include "alert_config.h"
#include "pg_types.h"

#include <stddef.h>

/*
 * M4 Alert & Governance — parser inih + catálogo inmutable de políticas.
 * Slice 4a Fase 2: carga el INI a estructuras tipadas; no toca procesos
 * ni state machine (eso llega con alert_state/alert_eval en fases 3-4).
 *
 * Scope de esta unidad de compilación:
 *  - pg_policy_catalog_load: parse + validación semántica multi-error.
 *  - pg_policy_catalog_destroy / pg_security_config_destroy: cleanup.
 */

typedef enum {
    PG_METRIC_UNKNOWN = 0,  /* zero-init seguro — nunca alcanza el catálogo */
    PG_METRIC_CPU_PERCENT,
    PG_METRIC_MEM_RSS,
    PG_METRIC_IO_READ_RATE,
    PG_METRIC_IO_WRITE_RATE
} pg_metric_id_t;

typedef enum {
    PG_POLICY_PERF = 0,     /* default si `type` ausente */
    PG_POLICY_SECURITY      /* eval desactivada en Slice 4; activa en Slice 5 */
} pg_policy_type_t;

typedef enum {
    PG_RISK_LOW = 0,
    PG_RISK_MEDIUM,         /* default si `risk` ausente */
    PG_RISK_HIGH,
    PG_RISK_CRITICAL
} pg_risk_level_t;

typedef enum {
    PG_ACT_WARN = 0,
    PG_ACT_RENICE,
    PG_ACT_AFFINITY,        /* parse OK; act() skip (Slice 4b)          */
    PG_ACT_CAGE,             /* parse OK; act() skip (Slice 4b)          */
    PG_ACT_STOP,
    PG_ACT_TERM,            /* parse OK; act() skip                     */
    PG_ACT_KILL
} pg_action_kind_t;

typedef struct {
    pg_action_kind_t kind;
    int              param; /* renice: nice nuevo; affinity: cpu mask    */
} pg_action_t;

typedef struct {
    char             name[64];     /* identificador único dentro del catálogo */
    pg_policy_type_t type;
    pg_risk_level_t  risk;
    pg_metric_id_t   metric;
    float            threshold;
    float            threshold_low;
    unsigned int     persistence;
    unsigned int     hysteresis_m;
    unsigned int     cooldown_s;
    pg_action_t     *actions;      /* owner: policy — libera en _destroy */
    size_t           n_actions;
} pg_policy_t;

/*
 * Carga el INI en `ini_path` a un catálogo inmutable de políticas, un
 * pg_global_config_t y un pg_security_config_t. Reporta TODOS los
 * errores a stderr antes de abortar (filosofía fail-loud).
 *
 * Parámetros:
 *   ini_path        (in)  ruta al archivo .ini; debe existir y ser legible.
 *   out_policies    (out) en éxito: malloc'd array de pg_policy_t de longitud
 *                         *out_n_policies; caller libera con _catalog_destroy.
 *                         En fallo: no se toca.
 *   out_n_policies  (out) cantidad de políticas (>=1 en éxito).
 *   out_global      (out) config global con defaults aplicados a las claves
 *                         ausentes del INI.
 *   out_security    (out) security config; vectores malloc'd (caller libera con
 *                         pg_security_config_destroy).
 *   out_n_errors    (out) opcional (NULL ok). Cantidad total de errores
 *                         acumulados durante parse + validación. En éxito: 0.
 *
 * Retorna:
 *   PG_OK         éxito; todos los out_* cargados.
 *   PG_ERR_PARSE  argumento NULL, archivo no abre, o ≥1 error semántico.
 *   PG_ERR_MEM    OOM en asignación interna.
 *
 * En fallo el caller NO debe invocar los destructores (no hay ownership
 * transferido).
 */
int pg_policy_catalog_load(const char           *ini_path,
                           pg_policy_t         **out_policies,
                           size_t               *out_n_policies,
                           pg_global_config_t   *out_global,
                           pg_security_config_t *out_security,
                           size_t               *out_n_errors);

/*
 * Libera un catálogo retornado por pg_policy_catalog_load. Idempotente
 * con policies == NULL (no-op, patrón free). n se ignora si policies es
 * NULL.
 */
void pg_policy_catalog_destroy(pg_policy_t *policies, size_t n);

/*
 * Libera los vectores dinámicos de pg_security_config_t. Zero-out los
 * punteros para que destroy doble sea seguro. NULL es no-op.
 */
void pg_security_config_destroy(pg_security_config_t *sec);

#endif /* PG_ALERT_POLICY_H */
