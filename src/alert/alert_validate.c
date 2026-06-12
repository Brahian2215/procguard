#include "alert_internal.h"

#include <string.h>

/* Rutas estándar por defecto cuando [security] no declara protected_paths.
 * Con barra final para evitar falsos prefijos (p.ej. "/usr/binX"). */
static const char *const DEFAULT_PROTECTED_PATHS[] = {
    "/usr/bin/", "/usr/sbin/", "/bin/", "/sbin/"
};
#define N_DEFAULT_PROTECTED_PATHS \
    (sizeof(DEFAULT_PROTECTED_PATHS) / sizeof(DEFAULT_PROTECTED_PATHS[0]))

static const pg_raw_sample_t *find_by_pid(const pg_raw_sample_t *s, size_t n,
                                          pid_t pid)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i].id.pid == pid) {
            return &s[i];
        }
    }
    return NULL;
}

static bool name_in_list(const char *comm, char *const *names, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (names[i] != NULL && strcmp(comm, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool has_prefix(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* exe en una ruta protegida: config si la hay, defaults si no. */
static bool exe_in_protected_path(const char *exe,
                                  const pg_security_config_t *sec)
{
    if (sec->n_protected_paths > 0) {
        for (size_t i = 0; i < sec->n_protected_paths; i++) {
            if (sec->protected_paths[i] != NULL &&
                has_prefix(exe, sec->protected_paths[i])) {
                return true;
            }
        }
        return false;
    }
    for (size_t i = 0; i < N_DEFAULT_PROTECTED_PATHS; i++) {
        if (has_prefix(exe, DEFAULT_PROTECTED_PATHS[i])) {
            return true;
        }
    }
    return false;
}

/* Whitelist ADR-012: retorna "protected" o NULL. */
static const char *whitelist_reason(const pg_alert_engine_t *eng,
                                    const pg_raw_sample_t *s)
{
    if (s->id.pid == 1 ||
        s->id.pid == eng->own_pid ||
        s->ppid == eng->own_pid ||
        s->exe_path[0] == '\0') {              /* kernel thread / muerto */
        return "protected";
    }
    if (name_in_list(s->comm, eng->security.protected_names,
                     eng->security.n_protected_names) &&
        exe_in_protected_path(s->exe_path, &eng->security)) {
        return "protected";
    }
    return NULL;
}

/* Cordura §7: STOP/KILL antes de 5s de alerta activa → bloquea. */
static bool sanity_blocks(pg_alert_engine_t *eng, const pg_alert_decision_t *d,
                          unsigned long long now)
{
    if (d->kind != PG_ACT_STOP && d->kind != PG_ACT_KILL) {
        return false;
    }
    pg_alert_state_t *st = pg_alert_state_lookup(eng->states, d->id,
                                                 d->policy_index);
    if (st == NULL || st->alert_active_since_ms == 0) {
        return false;
    }
    return (now - st->alert_active_since_ms) < 5000;
}

void pg_alert_validate(pg_alert_engine_t *eng,
                       const pg_raw_sample_t *samples, size_t n,
                       pg_alert_decision_t *decs, size_t n_dec,
                       unsigned long long now_ms)
{
    if (eng == NULL || decs == NULL) {
        return;
    }
    for (size_t i = 0; i < n_dec; i++) {
        pg_alert_decision_t *d = &decs[i];
        const pg_raw_sample_t *s = find_by_pid(samples, n, d->id.pid);
        if (s == NULL || s->id.starttime != d->id.starttime) {
            d->skip_reason = "stale_id";          /* ADR-005 */
            continue;
        }
        const char *wl = whitelist_reason(eng, s);
        if (wl != NULL) {
            d->skip_reason = wl;
            continue;
        }
        if (sanity_blocks(eng, d, now_ms)) {
            d->skip_reason = "sanity";
        }
    }
}
