#include "alert_internal.h"

#include "collector.h"     /* pg_collector_read_starttime (guard TOCTOU) */

#include <signal.h>        /* SIGSTOP, SIGKILL */
#include <stdbool.h>
#include <stdio.h>         /* fprintf */
#include <sys/resource.h>  /* PRIO_PROCESS */

/* Ventana del techo kills/min (ADR-014). */
#define KILL_WINDOW_MS 60000ULL

static const char *kind_str(pg_action_kind_t k)
{
    switch (k) {
    case PG_ACT_WARN:     return "WARN";
    case PG_ACT_RENICE:   return "RENICE";
    case PG_ACT_AFFINITY: return "AFFINITY";
    case PG_ACT_CAGE:     return "CAGE";
    case PG_ACT_STOP:     return "STOP";
    case PG_ACT_TERM:     return "TERM";
    case PG_ACT_KILL:     return "KILL";
    default:              return "?";
    }
}

/* Log plain a stderr (JSON lines llega con M7). Una línea por decisión. */
static void log_decision(const pg_alert_decision_t *d, const char *state)
{
    fprintf(stderr,
            "[alert] policy=%zu pid=%d metric=%.1f threshold=%.1f "
            "level=%d action=%s state=%s\n",
            d->policy_index, (int)d->id.pid, (double)d->metric_value,
            (double)d->threshold, d->level, kind_str(d->kind), state);
}

/* Avance vs reactivación (Fase 4) + cooldown + reset persistence. */
static void advance_after_act(pg_alert_state_t *st, const pg_policy_t *p,
                              unsigned long long now)
{
    st->cooldown_until_ms = now + (unsigned long long)p->cooldown_s * 1000ULL;
    st->persistence = 0;
    if (st->deactivated_since_last_act) {
        st->deactivated_since_last_act = false;   /* reactivación: mismo nivel */
    } else {
        st->escalation_level++;                    /* avance al próximo nivel */
    }
}

/* Registra un KILL ejecutado en el ring circular (techo kills/min, ADR-014). */
static void push_kill(pg_alert_engine_t *eng, unsigned long long now)
{
    if (eng->kill_ring_cap == 0) {
        return;
    }
    eng->kill_ring[eng->kill_ring_head] = now;
    eng->kill_ring_head = (eng->kill_ring_head + 1) % eng->kill_ring_cap;
    if (eng->kill_ring_count < eng->kill_ring_cap) {
        eng->kill_ring_count++;
    }
}

/* Cuenta los kills registrados dentro de [now-60s, now] (ADR-014). */
static size_t kills_last_minute(const pg_alert_engine_t *eng,
                                unsigned long long now)
{
    size_t count = 0;
    for (size_t i = 0; i < eng->kill_ring_count; i++) {
        unsigned long long ts = eng->kill_ring[i];
        if (ts <= now && now - ts < KILL_WINDOW_MS) {
            count++;
        }
    }
    return count;
}

/* Dispatch por kind. Retorna true si la acción cuenta para el escalamiento.
 * KILL con techo alcanzado → false (skip transitorio "ceiling", se reintenta;
 * NO avanza, a diferencia del no-stall). AFFINITY/CAGE/TERM no implementadas:
 * no syscall pero cuentan como ejecutadas (no-stall, ADR-014). */
static bool execute_action(pg_alert_engine_t *eng,
                           const pg_alert_decision_t *d, unsigned long long now)
{
    switch (d->kind) {
    case PG_ACT_RENICE:
        eng->sc.setpriority(PRIO_PROCESS, (id_t)d->id.pid, d->param);
        return true;
    case PG_ACT_STOP:
        eng->sc.kill(d->id.pid, SIGSTOP);
        return true;
    case PG_ACT_KILL:
        if (kills_last_minute(eng, now) >= eng->global.max_kills_per_minute) {
            return false;   /* techo: solo-alertas este ciclo */
        }
        eng->sc.kill(d->id.pid, SIGKILL);
        push_kill(eng, now);
        return true;
    case PG_ACT_WARN:
    case PG_ACT_AFFINITY:
    case PG_ACT_CAGE:
    case PG_ACT_TERM:
    default:
        return true;   /* WARN: log-only; no-stall: solo log */
    }
}

/* Guard TOCTOU (ADR-016): re-lee el starttime real justo antes del syscall.
 * Mismatch o proceso desaparecido → false (cancela sin avanzar). */
static bool toctou_ok(const pg_alert_engine_t *eng,
                      const pg_alert_decision_t *d)
{
    unsigned long long start = 0;
    int rc = pg_collector_read_starttime(eng->proc_base, d->id.pid, &start);
    return rc == PG_OK && start == d->id.starttime;
}

static bool is_destructive(pg_action_kind_t k)
{
    return k == PG_ACT_RENICE || k == PG_ACT_STOP || k == PG_ACT_KILL;
}

/* Índice del id en el registro de cageados, o -1. */
static int cage_index(const pg_alert_engine_t *eng, pg_proc_id_t id)
{
    for (size_t i = 0; i < eng->caged_count; i++) {
        if (eng->caged[i].pid == id.pid &&
            eng->caged[i].starttime == id.starttime) {
            return (int)i;
        }
    }
    return -1;
}

/* CAGE (ADR-018). Maneja su propio TOCTOU (solo antes de aplicar) y techo
 * no-stall. Retorna etiqueta de log; el caller avanza salvo skip:gone. */
static const char *cage_one(pg_alert_engine_t *eng, const pg_alert_decision_t *d,
                            pg_alert_state_t *st, const pg_policy_t *p,
                            unsigned long long now)
{
    if (eng->sc.cage_apply == NULL) {
        advance_after_act(st, p, now);
        return "cage_not_impl";                  /* sin backend → no-stall */
    }
    int idx = cage_index(eng, d->id);
    if (idx < 0 && eng->caged_count >= eng->global.max_caged_processes) {
        advance_after_act(st, p, now);
        return "caged_ceiling";                  /* cage lleno → no-stall */
    }
    if (!toctou_ok(eng, d)) {
        return "skip:gone";                      /* no avanza */
    }
    if (eng->sc.cage_apply(d->id.pid, eng->global.cage_cpu_percent) != PG_OK) {
        advance_after_act(st, p, now);
        return "cage_failed";                    /* fallo backend → no-stall */
    }
    if (idx < 0 && eng->caged_count < eng->caged_cap) {
        eng->caged[eng->caged_count++] = d->id;  /* contabiliza solo el nuevo */
    }
    advance_after_act(st, p, now);
    return "caged";
}

/* Aplica una decisión (skip_reason ya descartado). Retorna la etiqueta de
 * estado para el log. */
static const char *act_one(pg_alert_engine_t *eng, pg_alert_decision_t *d,
                           unsigned long long now)
{
    pg_alert_state_t *st = pg_alert_state_lookup(eng->states, d->id,
                                                 d->policy_index);
    if (st == NULL) {
        return "skip:no_state";   /* evaluate debió crearlo; defensivo */
    }
    const pg_policy_t *p = &eng->policies[d->policy_index];
    if (eng->global.dry_run) {
        advance_after_act(st, p, now);    /* previsualiza sin TOCTOU ni syscall */
        return "dry_run";
    }
    if (d->kind == PG_ACT_CAGE) {
        return cage_one(eng, d, st, p, now);   /* TOCTOU + techo propios (ADR-018) */
    }
    if (is_destructive(d->kind) && !toctou_ok(eng, d)) {
        return "skip:gone";               /* PID reciclado/desaparecido */
    }
    if (!execute_action(eng, d, now)) {
        return "skip:ceiling";            /* techo: no avanza, reintenta */
    }
    advance_after_act(st, p, now);
    return "executed";
}

void pg_alert_act(pg_alert_engine_t *eng,
                  pg_alert_decision_t *decs, size_t n_dec,
                  unsigned long long now_ms)
{
    if (eng == NULL || decs == NULL) {
        return;
    }
    for (size_t i = 0; i < n_dec; i++) {
        pg_alert_decision_t *d = &decs[i];
        if (d->skip_reason != NULL) {
            log_decision(d, d->skip_reason);
            continue;
        }
        log_decision(d, act_one(eng, d, now_ms));
    }
}
