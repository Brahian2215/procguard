#ifndef PG_ALERT_CONFIG_H
#define PG_ALERT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Configuración global del engine M4 (PDF §5.4). Campos triviales (sin
 * allocations) — se puede pasar por valor o por puntero. Defaults se
 * aplican en alert_policy.c cuando el INI no declara la clave.
 */
typedef struct {
    unsigned int sample_interval_ms;    /* default 500 */
    unsigned int sample_buffer;         /* default 120 */
    unsigned int max_kills_per_minute;  /* default 3   */
    unsigned int max_caged_processes;   /* default 10  */
    unsigned int cage_cpu_percent;      /* default 50 — cpu.max del cage (4c) */
    bool         dry_run;               /* default true */
} pg_global_config_t;

/*
 * Whitelist estática para M4 validate() y M5 security (entry de §7 Nivel 2
 * del PDF). Ambos vectores son dinámicos: el loader los llena con
 * `split_csv_trim` y el caller debe liberarlos con
 * pg_security_config_destroy. Zero-init seguro (todos ceros → no crashes).
 */
typedef struct {
    char   **protected_names; /* comms protegidos (p.ej. init, systemd, sshd) */
    size_t   n_protected_names;
    char   **protected_paths; /* prefixes de exe permitidos (p.ej. /usr/bin)  */
    size_t   n_protected_paths;
} pg_security_config_t;

#endif /* PG_ALERT_CONFIG_H */
