#ifndef PG_ALERT_INTERNAL_H
#define PG_ALERT_INTERNAL_H

#include "alert.h"
#include "alert_config.h"
#include "alert_policy.h"
#include "alert_state.h"
#include "pg_types.h"
#include "store.h"

#include <stddef.h>

/*
 * Definición completa del engine + tipo decisión + prototipos de las tres
 * pasadas (ADR-010). NO público: lo incluyen las unidades de alert*.c y los
 * tests, no los consumidores del engine (que ven alert.h opaco).
 */

/*
 * Decisión efímera producida por evaluate(); vive un solo ciclo (ADR-010).
 * skip_reason es NULL al salir de evaluate; validate() lo setea a un literal
 * estático (sin ownership) si la decisión debe omitirse.
 */
typedef struct {
    pg_proc_id_t     id;
    size_t           policy_index;
    pg_action_kind_t kind;          /* acción del nivel emitido */
    int              param;
    float            metric_value;  /* valor que disparó la decisión (log) */
    float            threshold;
    int              level;         /* escalation_level emitido */
    const char      *skip_reason;   /* NULL hasta validate() */
} pg_alert_decision_t;

struct pg_alert_engine {
    pg_policy_t               *policies;
    size_t                     n_policies;
    pg_global_config_t         global;
    pg_security_config_t       security;
    pg_alert_state_registry_t *states;
    pg_syscalls_t              sc;
    pid_t                      own_pid;
    long                       hz;
    long                       ncpus;
    /* Fase 6 (append-only): guard TOCTOU + techo kills/min (ADR-014/016). */
    char                      *proc_base;       /* copia propia; re-read TOCTOU */
    unsigned long long        *kill_ring;       /* timestamps de kills recientes */
    size_t                     kill_ring_cap;   /* = global.max_kills_per_minute */
    size_t                     kill_ring_count; /* slots ocupados (<= cap)        */
    size_t                     kill_ring_head;  /* próximo índice circular        */
    /* Slice 4c (ADR-018): registro de procesos cageados. cap =
     * max_caged_processes. cage_cpu_percent vive en global. */
    pg_proc_id_t              *caged;
    size_t                     caged_count;
    size_t                     caged_cap;
};

/*
 * Pasada 1 (Fase 4). Itera samples × catálogo aplicando la state machine
 * §5.4. Emite hasta `n * n_policies` decisiones en `out` (dimensionado por
 * el caller). Retorna la cantidad emitida.
 *
 * now_ms: reloj del ciclo (CLOCK_MONOTONIC ms); inyectado para tests.
 * Estados de (id,policy) sin sample en este ciclo → freeze (no se visitan).
 */
size_t pg_alert_evaluate(pg_alert_engine_t *eng,
                         const pg_raw_sample_t *samples, size_t n,
                         const pg_store_t *store,
                         unsigned long long now_ms,
                         pg_alert_decision_t *out);

/*
 * Dispatch métrica → valor actual (hueco A del plan). Mapea cada métrica del
 * catálogo a su lectura en la muestra actual:
 *   CPU_PERCENT     → pg_metrics_cpu_percent(prev,curr,..)  (sentinel -1.0f)
 *   IO_READ_RATE    → io_rates.read_bytes_per_s             (sentinel -1.0f)
 *   IO_WRITE_RATE   → io_rates.write_bytes_per_s            (sentinel -1.0f)
 *   MEM_RSS         → (float)curr->vmrss_bytes  (NO usa prev; nunca sentinel)
 * Métrica desconocida → -1.0f (defensivo; no debería alcanzar el catálogo).
 * `prev` puede ser NULL (primera muestra): las métricas delta devuelven
 * sentinel; MEM_RSS funciona igual.
 */
float pg_alert_metric_current(pg_metric_id_t metric,
                              const pg_raw_sample_t *prev,
                              const pg_raw_sample_t *curr,
                              long hz, long ncpus);

/*
 * Pasada 2 (Fase 5b). Pura: anota `skip_reason` (literal estático) sobre cada
 * decisión sin emitir syscalls. Orden, primer hit gana: `stale_id` (sample
 * del ciclo ausente o starttime distinto, ADR-005), `protected` (whitelist
 * ADR-012, usa `exe_path`), `sanity` (cordura 5s §7 para STOP/KILL). El techo
 * kills/min NO está aquí (vive en act, Fase 6, junto al ring).
 */
void pg_alert_validate(pg_alert_engine_t *eng,
                       const pg_raw_sample_t *samples, size_t n,
                       pg_alert_decision_t *decs, size_t n_dec,
                       unsigned long long now_ms);

/*
 * Pasada 3 (Fase 6). Aplica las decisiones con `skip_reason == NULL`:
 *   - Log primero (todas, incluidas skips y dry-run).
 *   - Guard TOCTOU (ADR-016): para RENICE/STOP/KILL re-lee el starttime vía
 *     pg_collector_read_starttime(eng->proc_base, pid); mismatch o proceso
 *     ausente → cancela sin avanzar nivel. El dry-run no necesita el guard
 *     (no hay syscall real que proteger).
 *   - Techo kills/min (ADR-014): KILL con kills_last_minute >= max → skip
 *     transitorio "ceiling" (no avanza, se reintenta); tras KILL ejecutado,
 *     push del timestamp al ring.
 *   - Dispatch por kind vía syscalls inyectados (ADR-009). AFFINITY/CAGE/TERM
 *     loguean "not implemented" pero AVANZAN nivel (no-stall, ADR-014).
 *   - dry_run: loguea "would <kind>", no ejecuta, pero avanza nivel/cooldown.
 *   - Tras acción contabilizada: cooldown + reset persistence + avance según
 *     deactivated_since_last_act (avance vs reactivación, Fase 4).
 */
void pg_alert_act(pg_alert_engine_t *eng,
                  pg_alert_decision_t *decs, size_t n_dec,
                  unsigned long long now_ms);

#endif /* PG_ALERT_INTERNAL_H */
