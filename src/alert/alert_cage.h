#ifndef PG_ALERT_CAGE_H
#define PG_ALERT_CAGE_H

#include <sys/types.h>

/*
 * Backend real de `cage` (cgroups v2, subárbol propio, ADR-014/018). Escribe
 * SOLO bajo /sys/fs/cgroup/procguard/<pid>/ (nunca toca cgroups del sistema).
 *
 * pg_cage_apply_sysfs: crea el subárbol del pid, fija cpu.max al `cpu_percent`%
 *   de un core (quota/period) y adjunta el pid vía cgroup.procs.
 * pg_cage_release_sysfs: rmdir del subárbol del pid (vacío tras su muerte).
 *
 * Best-effort: cualquier fallo (sin privilegios, controlador cpu no delegado,
 * proceso ausente) → PG_ERR_IO. act() lo trata como no-stall (ADR-014). Estos
 * son los punteros por defecto de pg_syscalls_t; los tests inyectan stubs.
 */
int pg_cage_apply_sysfs(pid_t pid, unsigned cpu_percent);
int pg_cage_release_sysfs(pid_t pid);

#endif /* PG_ALERT_CAGE_H */
