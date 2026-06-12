#include "alert_cage.h"

#include "pg_types.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>   /* mkdir */
#include <unistd.h>     /* rmdir */

/* Subárbol propio (ADR-014): el cage NUNCA escribe fuera de aquí. */
#define PG_CGROUP_ROOT    "/sys/fs/cgroup/procguard"
#define PG_CAGE_PERIOD_US 100000u   /* período cpu.max (100 ms) */

/* Escribe `content` (sin newline) a `path`. PG_OK / PG_ERR_IO. */
static int write_str(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return PG_ERR_IO;
    }
    int ok = (fputs(content, f) >= 0);
    if (fclose(f) != 0) {
        ok = 0;
    }
    return ok ? PG_OK : PG_ERR_IO;
}

/* mkdir idempotente (EEXIST no es error). PG_OK / PG_ERR_IO. */
static int ensure_dir(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        return PG_ERR_IO;
    }
    return PG_OK;
}

int pg_cage_apply_sysfs(pid_t pid, unsigned cpu_percent)
{
    char path[160];
    char buf[64];
    if (ensure_dir(PG_CGROUP_ROOT) != PG_OK) {
        return PG_ERR_IO;
    }
    /* Habilita el controlador cpu en el subárbol (best-effort: si la delegación
     * no está, cpu.max fallará abajo y devolvemos PG_ERR_IO → no-stall). */
    (void)write_str(PG_CGROUP_ROOT "/cgroup.subtree_control", "+cpu");
    snprintf(path, sizeof(path), PG_CGROUP_ROOT "/%d", (int)pid);
    if (ensure_dir(path) != PG_OK) {
        return PG_ERR_IO;
    }
    /* cpu.max = "quota period"; quota = cpu_percent% de un core. */
    snprintf(buf, sizeof(buf), "%u %u",
             cpu_percent * (PG_CAGE_PERIOD_US / 100u), PG_CAGE_PERIOD_US);
    snprintf(path, sizeof(path), PG_CGROUP_ROOT "/%d/cpu.max", (int)pid);
    if (write_str(path, buf) != PG_OK) {
        return PG_ERR_IO;
    }
    snprintf(buf, sizeof(buf), "%d", (int)pid);
    snprintf(path, sizeof(path), PG_CGROUP_ROOT "/%d/cgroup.procs", (int)pid);
    return write_str(path, buf);
}

int pg_cage_release_sysfs(pid_t pid)
{
    char path[160];
    snprintf(path, sizeof(path), PG_CGROUP_ROOT "/%d", (int)pid);
    return (rmdir(path) == 0) ? PG_OK : PG_ERR_IO;
}
