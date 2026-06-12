#include "alert_internal.h"

#include "metrics.h"

float pg_alert_metric_current(pg_metric_id_t metric,
                              const pg_raw_sample_t *prev,
                              const pg_raw_sample_t *curr,
                              long hz, long ncpus)
{
    switch (metric) {
    case PG_METRIC_CPU_PERCENT:
        return pg_metrics_cpu_percent(prev, curr, hz, ncpus);
    case PG_METRIC_MEM_RSS:
        /* Valor instantáneo: no usa prev, nunca sentinel (hueco A). */
        return (curr != NULL) ? (float)curr->vmrss_bytes : -1.0f;
    case PG_METRIC_IO_READ_RATE: {
        pg_io_rates_t r = { 0 };
        pg_metrics_io_rates(prev, curr, &r);
        return r.read_bytes_per_s;
    }
    case PG_METRIC_IO_WRITE_RATE: {
        pg_io_rates_t r = { 0 };
        pg_metrics_io_rates(prev, curr, &r);
        return r.write_bytes_per_s;
    }
    case PG_METRIC_UNKNOWN:
    default:
        return -1.0f;
    }
}

/* Paso 3 (§5.4): clasifica la métrica en tres zonas y ajusta contadores.
 * Banda muerta (threshold_low..threshold) rompe ambas rachas. */
static void apply_zone(pg_alert_state_t *st, const pg_policy_t *p,
                       float m, unsigned long long now)
{
    if (m > p->threshold) {
        st->persistence++;
        st->hysteresis = 0;
        if (st->persistence == p->persistence &&
            st->alert_active_since_ms == 0) {
            st->alert_active_since_ms = now;        /* §7 cordura */
        }
    } else if (m < p->threshold_low) {
        st->hysteresis++;
        st->persistence = 0;
    } else {
        st->persistence = 0;
        st->hysteresis = 0;
    }
}

/* Paso 4: desactivación por histéresis. No decrementa escalation_level
 * (avance vs reactivación lo decide act() vía deactivated_since_last_act). */
static bool maybe_deactivate(pg_alert_state_t *st, const pg_policy_t *p)
{
    if (st->hysteresis >= p->hysteresis_m && st->alert_active_since_ms != 0) {
        st->persistence = 0;
        st->hysteresis = 0;
        st->alert_active_since_ms = 0;
        st->deactivated_since_last_act = true;
        return true;
    }
    return false;
}

/* Paso 5: condición de emisión (persistencia + cooldown + nivel disponible). */
static bool should_emit(const pg_alert_state_t *st, const pg_policy_t *p,
                        unsigned long long now)
{
    return st->persistence >= p->persistence &&
           now >= st->cooldown_until_ms &&
           st->escalation_level < (int)p->n_actions;
}

static void fill_decision(pg_alert_decision_t *d, pg_proc_id_t id, size_t pi,
                          const pg_policy_t *p, const pg_alert_state_t *st,
                          float m)
{
    const pg_action_t *a = &p->actions[st->escalation_level];
    d->id = id;
    d->policy_index = pi;
    d->kind = a->kind;
    d->param = a->param;
    d->metric_value = m;
    d->threshold = p->threshold;
    d->level = st->escalation_level;
    d->skip_reason = NULL;            /* validate() (Fase 5) lo setea si aplica */
}

/* Evalúa un par (sample, policy). Retorna 1 si emitió una decisión, 0 si no. */
static size_t eval_one(pg_alert_engine_t *eng, const pg_raw_sample_t *curr,
                       const pg_raw_sample_t *prev, size_t pi,
                       unsigned long long now, pg_alert_decision_t *out)
{
    const pg_policy_t *p = &eng->policies[pi];
    if (p->type == PG_POLICY_SECURITY) {
        return 0;                                    /* paso 1: M5 en Slice 5 */
    }
    float m = pg_alert_metric_current(p->metric, prev, curr,
                                      eng->hz, eng->ncpus);
    if (m < 0.0f) {
        return 0;                                    /* paso 2: freeze */
    }
    pg_alert_state_t *st = NULL;
    if (pg_alert_state_upsert(eng->states, curr->id, pi, &st) != PG_OK) {
        return 0;                                    /* OOM: best-effort */
    }
    apply_zone(st, p, m, now);
    if (maybe_deactivate(st, p)) {
        return 0;
    }
    if (!should_emit(st, p, now)) {
        return 0;
    }
    fill_decision(out, curr->id, pi, p, st, m);
    return 1;
}

size_t pg_alert_evaluate(pg_alert_engine_t *eng,
                         const pg_raw_sample_t *samples, size_t n,
                         const pg_store_t *store,
                         unsigned long long now_ms,
                         pg_alert_decision_t *out)
{
    if (eng == NULL || out == NULL) {
        return 0;
    }
    size_t n_dec = 0;
    for (size_t i = 0; i < n; i++) {
        const pg_raw_sample_t *curr = &samples[i];
        pg_raw_sample_t hist[2];
        size_t hlen = 0;
        pg_store_get_history(store, curr->id, hist, 2, &hlen);
        const pg_raw_sample_t *prev = (hlen == 2) ? &hist[0] : NULL;
        for (size_t pi = 0; pi < eng->n_policies; pi++) {
            n_dec += eval_one(eng, curr, prev, pi, now_ms, &out[n_dec]);
        }
    }
    return n_dec;
}
