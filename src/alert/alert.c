#include "alert.h"
#include "alert_cage.h"    /* backend real de cage (ADR-018) */
#include "alert_internal.h"

#include <signal.h>        /* kill */
#include <stdlib.h>
#include <string.h>        /* memcpy, strlen */
#include <sys/resource.h>  /* setpriority */

/* glibc declara setpriority con un enum interno (__priority_which_t) como
 * primer parámetro; el puntero `int(*)(int,id_t,int)` no es asignable
 * directamente. Adaptador con la firma exacta del typedef. */
static int libc_setpriority(int which, id_t who, int prio)
{
    return setpriority(which, who, prio);
}

/* Syscalls por defecto: libc real (ADR-009). Los tests inyectan stubs. */
static pg_syscalls_t default_syscalls(void)
{
    pg_syscalls_t sc = { .kill = kill, .setpriority = libc_setpriority,
                         .cage_apply = pg_cage_apply_sysfs,
                         .cage_release = pg_cage_release_sysfs };
    return sc;
}

/* Copia owned de proc_base + ring de kills (cap = max_kills_per_minute, ya
 * cargado por el loader). cap == 0 → solo-alertas: sin ring, ceiling siempre.
 * Llamado tras cargar el catálogo. Retorna PG_OK / PG_ERR_MEM. */
static int alloc_runtime(pg_alert_engine_t *e, const char *proc_base)
{
    size_t len = strlen(proc_base);
    e->proc_base = malloc(len + 1);
    if (e->proc_base == NULL) {
        return PG_ERR_MEM;
    }
    memcpy(e->proc_base, proc_base, len + 1);
    e->kill_ring_cap = e->global.max_kills_per_minute;
    if (e->kill_ring_cap > 0) {
        e->kill_ring = calloc(e->kill_ring_cap, sizeof(*e->kill_ring));
        if (e->kill_ring == NULL) {
            return PG_ERR_MEM;
        }
    }
    e->caged_cap = e->global.max_caged_processes;
    if (e->caged_cap > 0) {
        e->caged = calloc(e->caged_cap, sizeof(*e->caged));
        if (e->caged == NULL) {
            return PG_ERR_MEM;
        }
    }
    return PG_OK;
}

int pg_alert_engine_init(pg_alert_engine_t **eng, const char *ini_path,
                         const char *proc_base, pid_t own_pid,
                         long hz, long ncpus, const pg_syscalls_t *sc)
{
    if (eng == NULL || ini_path == NULL || proc_base == NULL) {
        return PG_ERR_PARSE;
    }
    pg_alert_engine_t *e = calloc(1, sizeof(*e));
    if (e == NULL) {
        return PG_ERR_MEM;
    }
    int rc = pg_policy_catalog_load(ini_path, &e->policies, &e->n_policies,
                                    &e->global, &e->security, NULL);
    if (rc != PG_OK) {
        free(e);
        return rc;
    }
    rc = pg_alert_state_registry_init(&e->states);
    if (rc != PG_OK) {
        pg_policy_catalog_destroy(e->policies, e->n_policies);
        pg_security_config_destroy(&e->security);
        free(e);
        return rc;
    }
    e->own_pid = own_pid;
    e->hz = hz;
    e->ncpus = ncpus;
    e->sc = (sc != NULL) ? *sc : default_syscalls();
    rc = alloc_runtime(e, proc_base);
    if (rc != PG_OK) {
        pg_alert_engine_destroy(e);   /* libera todo lo asignado hasta aquí */
        return rc;
    }
    *eng = e;
    return PG_OK;
}

int pg_alert_engine_cycle(pg_alert_engine_t *eng,
                          const pg_raw_sample_t *samples, size_t n,
                          const pg_store_t *store, unsigned long long now_ms)
{
    if (eng == NULL) {
        return PG_ERR_PARSE;
    }
    size_t cap = n * eng->n_policies;
    pg_alert_decision_t *decs = NULL;
    if (cap > 0) {
        decs = calloc(cap, sizeof(*decs));
        if (decs == NULL) {
            return PG_ERR_MEM;
        }
    }
    /* ADR-010: lista efímera evaluate → validate → act, vive el ciclo. */
    size_t n_dec = pg_alert_evaluate(eng, samples, n, store, now_ms, decs);
    pg_alert_validate(eng, samples, n, decs, n_dec, now_ms);
    pg_alert_act(eng, decs, n_dec, now_ms);
    free(decs);
    return PG_OK;
}

/* ¿el id sigue en el store? (M2 ya aplicó la gracia G=10). */
static bool store_has(const pg_store_t *store, pg_proc_id_t id)
{
    pg_raw_sample_t one;
    size_t len = 0;
    pg_store_get_history(store, id, &one, 1, &len);
    return len > 0;
}

/* Libera (rmdir) los cages cuyos procesos ya no están en el store (ADR-018). */
static void cage_gc(pg_alert_engine_t *eng, const pg_store_t *store)
{
    for (size_t i = 0; i < eng->caged_count; ) {
        if (store_has(store, eng->caged[i])) {
            i++;
            continue;
        }
        if (eng->sc.cage_release != NULL) {
            eng->sc.cage_release(eng->caged[i].pid);
        }
        eng->caged[i] = eng->caged[--eng->caged_count];   /* swap-con-último */
    }
}

void pg_alert_engine_gc(pg_alert_engine_t *eng, const pg_store_t *store)
{
    if (eng == NULL) {
        return;
    }
    pg_alert_state_gc(eng->states, store);   /* ADR-013: tras store_tick */
    cage_gc(eng, store);
}

const pg_global_config_t *pg_alert_engine_global(const pg_alert_engine_t *eng)
{
    return (eng != NULL) ? &eng->global : NULL;
}

void pg_alert_engine_destroy(pg_alert_engine_t *eng)
{
    if (eng == NULL) {
        return;
    }
    pg_policy_catalog_destroy(eng->policies, eng->n_policies);
    pg_security_config_destroy(&eng->security);
    pg_alert_state_registry_destroy(eng->states);
    free(eng->proc_base);
    free(eng->kill_ring);
    free(eng->caged);
    free(eng);
}
