#ifndef PG_ALERT_H
#define PG_ALERT_H

#include "alert_config.h"
#include "pg_types.h"
#include "store.h"

#include <sys/types.h>

/*
 * M4 Alert & Governance — API pública del engine. El struct es opaco; la
 * definición completa y las pasadas internas (evaluate/validate/act) viven
 * en alert_internal.h, compartido por las unidades de compilación de alert
 * y por los tests.
 */
typedef struct pg_alert_engine pg_alert_engine_t;

/*
 * Operaciones con efecto inyectables (ADR-009/018). Por defecto apuntan a la
 * implementación real (libc kill/setpriority; backend sysfs de cage); los tests
 * inyectan stubs que cuentan invocaciones. cage_apply/cage_release en NULL
 * (test que solo inyecta kill/setpriority) → act() trata CAGE como no-stall.
 */
typedef struct {
    int (*kill)(pid_t, int);
    int (*setpriority)(int, id_t, int);
    int (*cage_apply)(pid_t pid, unsigned cpu_percent);  /* cgroups v2 (ADR-018) */
    int (*cage_release)(pid_t pid);
} pg_syscalls_t;

/*
 * Inicializa el engine cargando el catálogo de políticas desde `ini_path`
 * (vía pg_policy_catalog_load) y creando el registry de estados.
 *
 * proc_base  raíz de procfs (p.ej. "/proc"); el engine guarda una copia
 *            propia para el guard TOCTOU de act() (ADR-016, re-read starttime).
 * own_pid    PID del propio ProcGuard (whitelist runtime, ADR-012).
 * hz, ncpus  constantes del host para M3 (ADR-008).
 * sc         syscalls inyectados; NULL → libc por defecto.
 *
 * Retorna (propaga el código de pg_policy_catalog_load):
 *   PG_OK         éxito; *eng cargado (caller libera con _destroy).
 *   PG_ERR_PARSE  argumento NULL, o INI semánticamente inválido.
 *   PG_ERR_IO     INI no existe / no legible.
 *   PG_ERR_MEM    OOM.
 * En fallo *eng no se toca.
 */
int pg_alert_engine_init(pg_alert_engine_t **eng, const char *ini_path,
                         const char *proc_base, pid_t own_pid,
                         long hz, long ncpus, const pg_syscalls_t *sc);

/*
 * Ciclo de gobernanza M4 (ADR-010): evaluate → validate → act sobre una lista
 * efímera de decisiones (vive el ciclo). Aloja `n * n_policies` decisiones,
 * corre las tres pasadas y las libera. now_ms es el reloj del ciclo
 * (CLOCK_MONOTONIC ms). El integrador llama después store_tick + engine_gc
 * (ADR-013).
 *
 * Retorna:
 *   PG_OK         ciclo completo.
 *   PG_ERR_PARSE  eng == NULL.
 *   PG_ERR_MEM    OOM alojando la lista de decisiones.
 */
int pg_alert_engine_cycle(pg_alert_engine_t *eng,
                          const pg_raw_sample_t *samples, size_t n,
                          const pg_store_t *store, unsigned long long now_ms);

/*
 * Libera las entries del registry cuyos procesos ya no están en el store
 * (ADR-013). Debe llamarse TRAS pg_store_tick. NULL-safe.
 */
void pg_alert_engine_gc(pg_alert_engine_t *eng, const pg_store_t *store);

/*
 * Acceso de solo-lectura a la config `[global]` parseada (sample_interval,
 * sample_buffer, dry_run, techos). El orquestador la necesita para dimensionar
 * el store y el período del loop sin re-parsear el INI. El puntero es propiedad
 * del engine (válido hasta _destroy). NULL si eng == NULL.
 */
const pg_global_config_t *pg_alert_engine_global(const pg_alert_engine_t *eng);

/*
 * Libera el engine y todo lo que posee (catálogo, security config, registry
 * de estados, copia de proc_base, ring de kills). NULL-safe (no-op, free).
 */
void pg_alert_engine_destroy(pg_alert_engine_t *eng);

#endif /* PG_ALERT_H */
