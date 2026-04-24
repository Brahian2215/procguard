#include "alert_policy.h"

#include "ini.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ══════════════════════════════════════════════════════════════════
 * Estado de parseo (vive sólo durante pg_policy_catalog_load).
 * ══════════════════════════════════════════════════════════════════
 * policies/actions_raw son arrays paralelos: actions_raw[i] guarda la
 * lista cruda declarada en [policy:name_i] (strdup'd o NULL). Se
 * consume durante la validación para producir policies[i].actions.
 *
 * errors es un vector dinámico de strings malloc'd (reportados a
 * stderr y contados en *out_n_errors).
 *
 * oom es una bandera catastrófica: fuerza retorno inmediato y el load
 * retorna PG_ERR_MEM.
 */
typedef struct {
    pg_policy_t           *policies;
    char                 **actions_raw;
    size_t                 n_policies;
    size_t                 cap_policies;

    pg_global_config_t    *global;
    pg_security_config_t  *security;

    char                 **errors;
    size_t                 n_errors;
    size_t                 cap_errors;

    bool                   oom;
} parse_state_t;

/* ══════════════════════════════════════════════════════════════════
 * Error accumulator.
 * ══════════════════════════════════════════════════════════════════ */

static int ensure_errors_cap(parse_state_t *st)
{
    if (st->n_errors < st->cap_errors) return 0;
    size_t ncap = st->cap_errors ? st->cap_errors * 2 : 8;
    char **ne = realloc(st->errors, ncap * sizeof(*ne));
    if (!ne) { st->oom = true; return -1; }
    st->errors = ne;
    st->cap_errors = ncap;
    return 0;
}

static void push_errorf(parse_state_t *st, const char *fmt, ...)
{
    if (ensure_errors_cap(st) != 0) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char *dup = strdup(buf);
    if (!dup) { st->oom = true; return; }
    st->errors[st->n_errors++] = dup;
}

/* ══════════════════════════════════════════════════════════════════
 * String utilities.
 * ══════════════════════════════════════════════════════════════════ */

static char *str_trim_inplace(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    return s;
}

static char *str_dup_trim(const char *src)
{
    char *copy = strdup(src);
    if (!copy) return NULL;
    char *trimmed = str_trim_inplace(copy);
    if (trimmed != copy) memmove(copy, trimmed, strlen(trimmed) + 1);
    return copy;
}

static int parse_bool_strict(const char *s, bool *out)
{
    if (strcasecmp(s, "true") == 0 || strcmp(s, "1") == 0) { *out = true;  return 0; }
    if (strcasecmp(s, "false") == 0 || strcmp(s, "0") == 0) { *out = false; return 0; }
    return -1;
}

static int parse_uint_strict(const char *s, unsigned int *out)
{
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > (unsigned long)UINT_MAX) return -1;
    *out = (unsigned int)v;
    return 0;
}

static int parse_float_strict(const char *s, float *out)
{
    errno = 0;
    char *end = NULL;
    float v = strtof(s, &end);
    if (errno != 0 || end == s || *end != '\0') return -1;
    *out = v;
    return 0;
}

/* split_csv_trim: divide `s` por ',' y retorna un array de strdup's con
 * cada token trimmed. *n_out recibe la cantidad de tokens. Un token
 * vacío (ej. trailing comma o doble coma) retorna NULL y *n_out=0 con
 * errno=EINVAL para que el caller reporte error. El caller libera con
 * free_string_vector. */
static void free_string_vector(char **v, size_t n)
{
    if (!v) return;
    for (size_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

static char **split_csv_trim(const char *s, size_t *n_out)
{
    *n_out = 0;
    char **out = NULL;
    size_t n = 0, cap = 0;
    const char *p = s;
    while (1) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        char *tok = malloc(len + 1);
        if (!tok) { free_string_vector(out, n); return NULL; }
        memcpy(tok, p, len);
        tok[len] = '\0';
        char *trimmed = str_trim_inplace(tok);
        if (trimmed != tok) memmove(tok, trimmed, strlen(trimmed) + 1);
        if (tok[0] == '\0') {
            free(tok); free_string_vector(out, n);
            errno = EINVAL; return NULL;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            char **nv = realloc(out, cap * sizeof(*nv));
            if (!nv) { free(tok); free_string_vector(out, n); return NULL; }
            out = nv;
        }
        out[n++] = tok;
        if (!comma) break;
        p = comma + 1;
    }
    *n_out = n;
    return out;
}

/* ══════════════════════════════════════════════════════════════════
 * Lookup tables.
 * ══════════════════════════════════════════════════════════════════ */

static int lookup_metric(const char *s, pg_metric_id_t *out)
{
    if (strcmp(s, "cpu_percent") == 0)    { *out = PG_METRIC_CPU_PERCENT;   return 0; }
    if (strcmp(s, "mem_rss") == 0)        { *out = PG_METRIC_MEM_RSS;       return 0; }
    if (strcmp(s, "io_read_rate") == 0)   { *out = PG_METRIC_IO_READ_RATE;  return 0; }
    if (strcmp(s, "io_write_rate") == 0)  { *out = PG_METRIC_IO_WRITE_RATE; return 0; }
    return -1;
}

static int lookup_policy_type(const char *s, pg_policy_type_t *out)
{
    if (strcasecmp(s, "performance") == 0 || strcasecmp(s, "perf") == 0) {
        *out = PG_POLICY_PERF; return 0;
    }
    if (strcasecmp(s, "security") == 0) { *out = PG_POLICY_SECURITY; return 0; }
    return -1;
}

static int lookup_risk(const char *s, pg_risk_level_t *out)
{
    if (strcasecmp(s, "low") == 0)      { *out = PG_RISK_LOW;      return 0; }
    if (strcasecmp(s, "medium") == 0)   { *out = PG_RISK_MEDIUM;   return 0; }
    if (strcasecmp(s, "high") == 0)     { *out = PG_RISK_HIGH;     return 0; }
    if (strcasecmp(s, "critical") == 0) { *out = PG_RISK_CRITICAL; return 0; }
    return -1;
}

static int lookup_action_kind(const char *s, pg_action_kind_t *out)
{
    if (strcasecmp(s, "warn") == 0)     { *out = PG_ACT_WARN;     return 0; }
    if (strcasecmp(s, "renice") == 0)   { *out = PG_ACT_RENICE;   return 0; }
    if (strcasecmp(s, "affinity") == 0) { *out = PG_ACT_AFFINITY; return 0; }
    if (strcasecmp(s, "cage") == 0)     { *out = PG_ACT_CAGE;     return 0; }
    if (strcasecmp(s, "stop") == 0)     { *out = PG_ACT_STOP;     return 0; }
    if (strcasecmp(s, "term") == 0)     { *out = PG_ACT_TERM;     return 0; }
    if (strcasecmp(s, "kill") == 0)     { *out = PG_ACT_KILL;     return 0; }
    return -1;
}

static bool action_kind_requires_param(pg_action_kind_t k)
{
    return k == PG_ACT_RENICE || k == PG_ACT_AFFINITY;
}

/* ══════════════════════════════════════════════════════════════════
 * Policy lookup + creation.
 * ══════════════════════════════════════════════════════════════════ */

static void init_policy_defaults(pg_policy_t *p, const char *name)
{
    memset(p, 0, sizeof(*p));
    strncpy(p->name, name, sizeof(p->name) - 1);
    p->type          = PG_POLICY_PERF;
    p->risk          = PG_RISK_MEDIUM;
    p->metric        = PG_METRIC_UNKNOWN;
    p->threshold     = 0.0f;
    p->threshold_low = -1.0f;  /* sentinel: "no seteado"; se copia de threshold si sigue -1 */
    p->persistence   = 1;
    p->hysteresis_m  = 1;
    p->cooldown_s    = 0;
}

static pg_policy_t *find_policy(parse_state_t *st, const char *name)
{
    for (size_t i = 0; i < st->n_policies; i++) {
        if (strcmp(st->policies[i].name, name) == 0) return &st->policies[i];
    }
    return NULL;
}

static pg_policy_t *grow_policies_and_push(parse_state_t *st, const char *name)
{
    if (st->n_policies == st->cap_policies) {
        size_t ncap = st->cap_policies ? st->cap_policies * 2 : 8;
        pg_policy_t *np = realloc(st->policies, ncap * sizeof(*np));
        if (!np) { st->oom = true; return NULL; }
        st->policies = np;
        char **nar = realloc(st->actions_raw, ncap * sizeof(*nar));
        if (!nar) { st->oom = true; return NULL; }
        st->actions_raw = nar;
        st->cap_policies = ncap;
    }
    pg_policy_t *p = &st->policies[st->n_policies];
    init_policy_defaults(p, name);
    st->actions_raw[st->n_policies] = NULL;
    st->n_policies++;
    return p;
}

static pg_policy_t *get_or_create_policy(parse_state_t *st, const char *name)
{
    pg_policy_t *existing = find_policy(st, name);
    if (existing) return existing;
    return grow_policies_and_push(st, name);
}

/* ══════════════════════════════════════════════════════════════════
 * Section handlers — [global], [security], [policy:*].
 * ══════════════════════════════════════════════════════════════════ */

static void handle_global(parse_state_t *st, const char *name, const char *value)
{
    if (strcmp(name, "sample_interval") == 0) {
        if (parse_uint_strict(value, &st->global->sample_interval_ms) != 0)
            push_errorf(st, "[global] sample_interval: invalid uint '%s'", value);
    } else if (strcmp(name, "sample_buffer") == 0) {
        if (parse_uint_strict(value, &st->global->sample_buffer) != 0)
            push_errorf(st, "[global] sample_buffer: invalid uint '%s'", value);
    } else if (strcmp(name, "max_kills_per_minute") == 0) {
        if (parse_uint_strict(value, &st->global->max_kills_per_minute) != 0)
            push_errorf(st, "[global] max_kills_per_minute: invalid uint '%s'", value);
    } else if (strcmp(name, "max_caged_processes") == 0) {
        if (parse_uint_strict(value, &st->global->max_caged_processes) != 0)
            push_errorf(st, "[global] max_caged_processes: invalid uint '%s'", value);
    } else if (strcmp(name, "dry_run") == 0) {
        if (parse_bool_strict(value, &st->global->dry_run) != 0)
            push_errorf(st, "[global] dry_run: invalid bool '%s'", value);
    } else {
        push_errorf(st, "[global] unknown key: %s", name);
    }
}

static int handle_security_vector(const char *value, char ***target, size_t *n_target,
                                  parse_state_t *st, const char *field)
{
    size_t n = 0;
    char **vec = split_csv_trim(value, &n);
    if (!vec) {
        if (errno == EINVAL) push_errorf(st, "[security] %s: empty token", field);
        else st->oom = true;
        return -1;
    }
    free_string_vector(*target, *n_target);
    *target = vec;
    *n_target = n;
    return 0;
}

static void handle_security(parse_state_t *st, const char *name, const char *value)
{
    if (strcmp(name, "protected_names") == 0) {
        handle_security_vector(value, &st->security->protected_names,
                               &st->security->n_protected_names, st, "protected_names");
    } else if (strcmp(name, "protected_paths") == 0) {
        handle_security_vector(value, &st->security->protected_paths,
                               &st->security->n_protected_paths, st, "protected_paths");
    } else {
        push_errorf(st, "[security] unknown key: %s", name);
    }
}

static void handle_policy_scalar(parse_state_t *st, pg_policy_t *p,
                                 const char *name, const char *value)
{
    if (strcmp(name, "type") == 0) {
        if (lookup_policy_type(value, &p->type) != 0)
            push_errorf(st, "policy '%s': unknown type '%s'", p->name, value);
    } else if (strcmp(name, "risk") == 0) {
        if (lookup_risk(value, &p->risk) != 0)
            push_errorf(st, "policy '%s': unknown risk '%s'", p->name, value);
    } else if (strcmp(name, "metric") == 0) {
        /* Unknown metric ⇒ deja p->metric en PG_METRIC_UNKNOWN sin pushear
         * error. La validación post-parse emite un único error unificado
         * que también cubre "metric missing" (nunca declarado). Evita
         * doble-conteo en tests de multi-error. */
        (void)lookup_metric(value, &p->metric);
    } else if (strcmp(name, "threshold") == 0) {
        if (parse_float_strict(value, &p->threshold) != 0)
            push_errorf(st, "policy '%s': threshold invalid float '%s'", p->name, value);
    } else if (strcmp(name, "threshold_low") == 0) {
        if (parse_float_strict(value, &p->threshold_low) != 0)
            push_errorf(st, "policy '%s': threshold_low invalid float '%s'", p->name, value);
    } else if (strcmp(name, "persistence") == 0) {
        if (parse_uint_strict(value, &p->persistence) != 0)
            push_errorf(st, "policy '%s': persistence invalid uint '%s'", p->name, value);
    } else if (strcmp(name, "hysteresis_m") == 0) {
        if (parse_uint_strict(value, &p->hysteresis_m) != 0)
            push_errorf(st, "policy '%s': hysteresis_m invalid uint '%s'", p->name, value);
    } else if (strcmp(name, "cooldown_s") == 0) {
        if (parse_uint_strict(value, &p->cooldown_s) != 0)
            push_errorf(st, "policy '%s': cooldown_s invalid uint '%s'", p->name, value);
    } else {
        push_errorf(st, "policy '%s': unknown key '%s'", p->name, name);
    }
}

static void handle_policy(parse_state_t *st, pg_policy_t *p,
                          const char *name, const char *value)
{
    if (strcmp(name, "actions") == 0) {
        size_t idx = (size_t)(p - st->policies);
        free(st->actions_raw[idx]);
        st->actions_raw[idx] = str_dup_trim(value);
        if (!st->actions_raw[idx]) { st->oom = true; return; }
        return;
    }
    handle_policy_scalar(st, p, name, value);
}

/* ══════════════════════════════════════════════════════════════════
 * Top-level ini_handler dispatch.
 * ══════════════════════════════════════════════════════════════════ */

static int ini_cb(void *user, const char *section, const char *name, const char *value)
{
    parse_state_t *st = (parse_state_t *)user;
    if (st->oom) return 0;

    if (strcmp(section, "global") == 0) {
        handle_global(st, name, value);
    } else if (strcmp(section, "security") == 0) {
        handle_security(st, name, value);
    } else if (strncmp(section, "policy:", 7) == 0) {
        const char *pname = section + 7;
        if (pname[0] == '\0') {
            push_errorf(st, "empty policy name in section header");
        } else {
            pg_policy_t *p = get_or_create_policy(st, pname);
            if (p) handle_policy(st, p, name, value);
        }
    } else {
        push_errorf(st, "unknown section: [%s]", section);
    }
    return st->oom ? 0 : 1;
}

/* ══════════════════════════════════════════════════════════════════
 * Validación post-parse.
 * ══════════════════════════════════════════════════════════════════ */

static int parse_single_action(const char *item, pg_action_t *out_act, char **err_out)
{
    *err_out = NULL;
    char *copy = strdup(item);
    if (!copy) return -1;
    char *colon = strchr(copy, ':');
    if (colon) *colon = '\0';
    char *kind_s = str_trim_inplace(copy);
    pg_action_kind_t kind;
    if (lookup_action_kind(kind_s, &kind) != 0) {
        *err_out = strdup("unknown action kind");
        free(copy); return -1;
    }
    int param = 0;
    if (colon) {
        char *param_s = str_trim_inplace(colon + 1);
        if (!action_kind_requires_param(kind)) {
            *err_out = strdup("action does not accept a param");
            free(copy); return -1;
        }
        unsigned int tmp;
        if (parse_uint_strict(param_s, &tmp) != 0) {
            /* Aceptar negativos para renice (nice -20..19). */
            errno = 0;
            char *end = NULL;
            long v = strtol(param_s, &end, 10);
            if (errno != 0 || end == param_s || *end != '\0') {
                *err_out = strdup("invalid action param"); free(copy); return -1;
            }
            param = (int)v;
        } else {
            param = (int)tmp;
        }
    } else if (action_kind_requires_param(kind)) {
        *err_out = strdup("action requires a param");
        free(copy); return -1;
    }
    out_act->kind = kind;
    out_act->param = param;
    free(copy);
    return 0;
}

static int validate_policy_actions(parse_state_t *st, pg_policy_t *p, size_t idx)
{
    char *raw = st->actions_raw[idx];
    if (!raw || raw[0] == '\0') {
        push_errorf(st, "policy '%s': actions list is empty or missing", p->name);
        return -1;
    }
    size_t n_tokens = 0;
    char **tokens = split_csv_trim(raw, &n_tokens);
    if (!tokens) {
        if (errno == EINVAL)
            push_errorf(st, "policy '%s': actions contains empty token", p->name);
        else st->oom = true;
        return -1;
    }
    pg_action_t *acts = calloc(n_tokens, sizeof(*acts));
    if (!acts) { free_string_vector(tokens, n_tokens); st->oom = true; return -1; }
    for (size_t i = 0; i < n_tokens; i++) {
        char *err = NULL;
        if (parse_single_action(tokens[i], &acts[i], &err) != 0) {
            push_errorf(st, "policy '%s': action '%s': %s",
                        p->name, tokens[i], err ? err : "parse error");
            free(err);
            free_string_vector(tokens, n_tokens);
            free(acts);
            return -1;
        }
    }
    free_string_vector(tokens, n_tokens);
    p->actions = acts;
    p->n_actions = n_tokens;
    return 0;
}

static void validate_policy_fields(parse_state_t *st, pg_policy_t *p)
{
    if (p->metric == PG_METRIC_UNKNOWN)
        push_errorf(st, "policy '%s': metric unknown or missing", p->name);
    if (p->threshold_low < 0.0f) p->threshold_low = p->threshold; /* apply default */
    if (p->threshold_low > p->threshold)
        push_errorf(st, "policy '%s': threshold_low (%g) > threshold (%g)",
                    p->name, (double)p->threshold_low, (double)p->threshold);
    if (p->persistence < 1)
        push_errorf(st, "policy '%s': persistence must be >= 1", p->name);
}

static void check_duplicate_names(parse_state_t *st)
{
    for (size_t i = 0; i < st->n_policies; i++) {
        for (size_t j = i + 1; j < st->n_policies; j++) {
            if (strcmp(st->policies[i].name, st->policies[j].name) == 0) {
                push_errorf(st, "duplicate policy name: '%s'", st->policies[i].name);
            }
        }
    }
}

static void validate_all(parse_state_t *st)
{
    check_duplicate_names(st);
    for (size_t i = 0; i < st->n_policies; i++) {
        validate_policy_fields(st, &st->policies[i]);
        validate_policy_actions(st, &st->policies[i], i);
    }
    if (st->n_policies == 0) push_errorf(st, "no policies defined in INI");
}

/* ══════════════════════════════════════════════════════════════════
 * Lifecycle — init, free_state_partial, apply_global_defaults.
 * ══════════════════════════════════════════════════════════════════ */

static void apply_global_defaults(pg_global_config_t *g)
{
    g->sample_interval_ms   = 500;
    g->sample_buffer        = 120;
    g->max_kills_per_minute = 3;
    g->max_caged_processes  = 10;
    g->dry_run              = true;
}

static void free_partial_catalog(parse_state_t *st)
{
    for (size_t i = 0; i < st->n_policies; i++) {
        free(st->policies[i].actions);
        free(st->actions_raw[i]);
    }
    free(st->policies);
    free(st->actions_raw);
    st->policies = NULL;
    st->actions_raw = NULL;
    st->n_policies = st->cap_policies = 0;
}

static void free_errors(parse_state_t *st)
{
    for (size_t i = 0; i < st->n_errors; i++) free(st->errors[i]);
    free(st->errors);
    st->errors = NULL;
    st->n_errors = st->cap_errors = 0;
}

static void print_errors_stderr(parse_state_t *st, const char *ini_path)
{
    for (size_t i = 0; i < st->n_errors; i++) {
        fprintf(stderr, "[policy-config] %s: %s\n", ini_path, st->errors[i]);
    }
}

/* ══════════════════════════════════════════════════════════════════
 * API pública.
 * ══════════════════════════════════════════════════════════════════ */

static int abort_with_errors(parse_state_t *st, const char *ini_path,
                             pg_global_config_t *g, pg_security_config_t *sec,
                             size_t *out_n_errors)
{
    print_errors_stderr(st, ini_path);
    if (out_n_errors) *out_n_errors = st->n_errors;
    int rc = st->oom ? PG_ERR_MEM : PG_ERR_PARSE;
    free_partial_catalog(st);
    free_errors(st);
    pg_security_config_destroy(sec);
    apply_global_defaults(g);
    return rc;
}

static void finalize_success(parse_state_t *st, pg_policy_t **out_policies,
                             size_t *out_n_policies, size_t *out_n_errors)
{
    for (size_t i = 0; i < st->n_policies; i++) free(st->actions_raw[i]);
    free(st->actions_raw);
    free_errors(st);
    *out_policies = st->policies;
    *out_n_policies = st->n_policies;
    if (out_n_errors) *out_n_errors = 0;
}

int pg_policy_catalog_load(const char           *ini_path,
                           pg_policy_t         **out_policies,
                           size_t               *out_n_policies,
                           pg_global_config_t   *out_global,
                           pg_security_config_t *out_security,
                           size_t               *out_n_errors)
{
    if (!ini_path || !out_policies || !out_n_policies || !out_global || !out_security)
        return PG_ERR_PARSE;
    apply_global_defaults(out_global);
    memset(out_security, 0, sizeof(*out_security));

    parse_state_t st = {0};
    st.global = out_global;
    st.security = out_security;

    int rc_ini = ini_parse(ini_path, ini_cb, &st);
    if (rc_ini == -1) {
        free_partial_catalog(&st); free_errors(&st);
        pg_security_config_destroy(out_security);
        return PG_ERR_IO;
    }
    if (st.oom)
        return abort_with_errors(&st, ini_path, out_global, out_security, out_n_errors);

    validate_all(&st);

    if (st.oom || st.n_errors > 0)
        return abort_with_errors(&st, ini_path, out_global, out_security, out_n_errors);

    finalize_success(&st, out_policies, out_n_policies, out_n_errors);
    return PG_OK;
}

void pg_policy_catalog_destroy(pg_policy_t *policies, size_t n)
{
    if (!policies) return;
    for (size_t i = 0; i < n; i++) free(policies[i].actions);
    free(policies);
}

void pg_security_config_destroy(pg_security_config_t *sec)
{
    if (!sec) return;
    free_string_vector(sec->protected_names, sec->n_protected_names);
    free_string_vector(sec->protected_paths, sec->n_protected_paths);
    sec->protected_names = NULL;
    sec->n_protected_names = 0;
    sec->protected_paths = NULL;
    sec->n_protected_paths = 0;
}
