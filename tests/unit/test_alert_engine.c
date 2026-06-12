#include "unity.h"

#include "alert.h"
#include "alert_internal.h"
#include "pg_types.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Cada test que necesita un INI lo escribe vía mkstemp y lo unlinkea en
 * tearDown (mismo patrón que test_alert_parser). */
static char g_tmp_path[64];

/* proc_base por defecto para tests que no ejercitan el guard TOCTOU de act
 * (eval/validate nunca llaman read_starttime). Los tests de act montan un
 * procfs sintético propio y pasan su raíz. */
#define TEST_PROC_BASE "/proc"

void setUp(void)
{
    g_tmp_path[0] = '\0';
}

void tearDown(void)
{
    if (g_tmp_path[0] != '\0') {
        unlink(g_tmp_path);
        g_tmp_path[0] = '\0';
    }
    /* Limpia el procfs sintético de los tests de act (residuo entre tests). */
    (void)system("rm -rf /tmp/pg_test_act_proc");
}

static void write_temp_ini(const char *contents)
{
    strcpy(g_tmp_path, "/tmp/pg_test_engine_XXXXXX");
    int fd = mkstemp(g_tmp_path);
    TEST_ASSERT_TRUE_MESSAGE(fd >= 0, "mkstemp failed");
    size_t len = strlen(contents);
    ssize_t n = write(fd, contents, len);
    TEST_ASSERT_EQUAL_INT((int)len, (int)n);
    close(fd);
}

static const char *VALID_INI =
    "[global]\n"
    "dry_run = true\n"
    "\n"
    "[policy:cpu_hog]\n"
    "type = performance\n"
    "metric = cpu_percent\n"
    "threshold = 80.0\n"
    "threshold_low = 60.0\n"
    "persistence = 3\n"
    "hysteresis_m = 2\n"
    "cooldown_s = 10\n"
    "actions = warn, renice:10, stop, kill\n";

/* ── init carga el catálogo y aplica defaults globales ─────────────── */
static void test_init_loads_policies_and_defaults(void)
{
    write_temp_ini(VALID_INI);

    pg_alert_engine_t *eng = NULL;
    int rc = pg_alert_engine_init(&eng, g_tmp_path, TEST_PROC_BASE,
                                  4242, 100, 4, NULL);

    TEST_ASSERT_EQUAL_INT(PG_OK, rc);
    TEST_ASSERT_NOT_NULL(eng);
    TEST_ASSERT_EQUAL_UINT(1, eng->n_policies);
    TEST_ASSERT_EQUAL_INT(PG_METRIC_CPU_PERCENT, eng->policies[0].metric);
    TEST_ASSERT_TRUE(eng->global.dry_run);          /* default */
    TEST_ASSERT_EQUAL_INT(4242, eng->own_pid);
    TEST_ASSERT_EQUAL_INT(100, eng->hz);
    TEST_ASSERT_EQUAL_INT(4, eng->ncpus);
    TEST_ASSERT_NOT_NULL(eng->states);
    TEST_ASSERT_NOT_NULL(eng->sc.kill);             /* libc por default */
    TEST_ASSERT_NOT_NULL(eng->sc.setpriority);

    pg_alert_engine_destroy(eng);
}

/* ── args NULL rechazados ──────────────────────────────────────────── */
static void test_init_null_args_return_parse_err(void)
{
    pg_alert_engine_t *eng = NULL;
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE,
        pg_alert_engine_init(NULL, "x", TEST_PROC_BASE, 1, 100, 4, NULL));
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE,
        pg_alert_engine_init(&eng, NULL, TEST_PROC_BASE, 1, 100, 4, NULL));
    TEST_ASSERT_NULL(eng);
}

/* ── init propaga el código del loader: INI inexistente → PG_ERR_IO ── */
static void test_init_unreadable_ini_returns_io_err(void)
{
    pg_alert_engine_t *eng = NULL;
    int rc = pg_alert_engine_init(&eng, "/tmp/pg_does_not_exist_99999.ini",
                                  TEST_PROC_BASE, 1, 100, 4, NULL);
    TEST_ASSERT_EQUAL_INT(PG_ERR_IO, rc);
    TEST_ASSERT_NULL(eng);
}

/* ── init propaga PG_ERR_PARSE ante contenido semánticamente inválido ─ */
static void test_init_invalid_ini_returns_parse_err(void)
{
    /* policy sin `actions` (obligatorio, ADR-011) → error semántico. */
    write_temp_ini(
        "[policy:bad]\n"
        "type = performance\n"
        "metric = cpu_percent\n"
        "threshold = 80.0\n"
        "threshold_low = 60.0\n");

    pg_alert_engine_t *eng = NULL;
    int rc = pg_alert_engine_init(&eng, g_tmp_path, TEST_PROC_BASE,
                                  1, 100, 4, NULL);
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, rc);
    TEST_ASSERT_NULL(eng);
}

/* ── custom syscalls se almacenan tal cual ─────────────────────────── */
static int stub_kill(pid_t pid, int sig) { (void)pid; (void)sig; return 0; }
static int stub_setpriority(int w, id_t who, int prio)
{ (void)w; (void)who; (void)prio; return 0; }

static void test_init_stores_injected_syscalls(void)
{
    write_temp_ini(VALID_INI);
    pg_syscalls_t sc = { .kill = stub_kill, .setpriority = stub_setpriority };

    pg_alert_engine_t *eng = NULL;
    int rc = pg_alert_engine_init(&eng, g_tmp_path, TEST_PROC_BASE,
                                  1, 100, 4, &sc);
    TEST_ASSERT_EQUAL_INT(PG_OK, rc);
    TEST_ASSERT_TRUE(eng->sc.kill == stub_kill);
    TEST_ASSERT_TRUE(eng->sc.setpriority == stub_setpriority);

    pg_alert_engine_destroy(eng);
}

/* ── init copia proc_base y aloja el ring de kills (Fase 6) ─────────── */
static void test_init_copies_proc_base_and_allocs_ring(void)
{
    write_temp_ini(VALID_INI);   /* sin max_kills_per_minute → default 3 */
    char base[16];
    strcpy(base, "/proc");
    pg_alert_engine_t *eng = NULL;
    int rc = pg_alert_engine_init(&eng, g_tmp_path, base, 4242, 100, 4, NULL);
    TEST_ASSERT_EQUAL_INT(PG_OK, rc);
    TEST_ASSERT_NOT_NULL(eng->proc_base);
    strcpy(base, "/MUTATED");                  /* el engine guarda copia propia */
    TEST_ASSERT_EQUAL_STRING("/proc", eng->proc_base);
    TEST_ASSERT_EQUAL_UINT(3, eng->global.max_kills_per_minute);
    TEST_ASSERT_EQUAL_UINT(3, eng->kill_ring_cap);
    TEST_ASSERT_NOT_NULL(eng->kill_ring);
    TEST_ASSERT_EQUAL_UINT(0, eng->kill_ring_count);
    TEST_ASSERT_EQUAL_UINT(0, eng->kill_ring_head);
    pg_alert_engine_destroy(eng);
}

/* ── default_syscalls cablea el backend de cage (Fase 2, ADR-018) ──── */
static void test_default_syscalls_wires_cage(void)
{
    write_temp_ini(VALID_INI);
    pg_alert_engine_t *eng = NULL;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_alert_engine_init(
        &eng, g_tmp_path, TEST_PROC_BASE, 4242, 100, 4, NULL));  /* sc NULL */
    TEST_ASSERT_NOT_NULL(eng->sc.cage_apply);
    TEST_ASSERT_NOT_NULL(eng->sc.cage_release);
    pg_alert_engine_destroy(eng);
}

/* ── proc_base NULL es argumento inválido ──────────────────────────── */
static void test_init_null_proc_base_returns_parse_err(void)
{
    write_temp_ini(VALID_INI);
    pg_alert_engine_t *eng = NULL;
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE,
        pg_alert_engine_init(&eng, g_tmp_path, NULL, 1, 100, 4, NULL));
    TEST_ASSERT_NULL(eng);
}

/* ── metric_current: dispatch por métrica ──────────────────────────── */

/* Construye una muestra mínima con id e instrumentos para deltas. */
static pg_raw_sample_t mk_sample(pid_t pid, unsigned long long start,
                                 unsigned long long ts_ms,
                                 unsigned long long utime,
                                 unsigned long long stime)
{
    pg_raw_sample_t s = { 0 };
    s.id.pid = pid;
    s.id.starttime = start;
    s.timestamp_ms = ts_ms;
    s.utime = utime;
    s.stime = stime;
    return s;
}

static void test_metric_cpu_percent_maps_to_m3(void)
{
    /* hz=100: 100 jiffies en 1s = 1 CPU-segundo = 100% de 1 core. */
    pg_raw_sample_t prev = mk_sample(7, 1, 0, 100, 0);
    pg_raw_sample_t curr = mk_sample(7, 1, 1000, 200, 0);
    float v = pg_alert_metric_current(PG_METRIC_CPU_PERCENT, &prev, &curr, 100, 4);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, v);
}

static void test_metric_cpu_percent_null_prev_is_sentinel(void)
{
    pg_raw_sample_t curr = mk_sample(7, 1, 1000, 200, 0);
    float v = pg_alert_metric_current(PG_METRIC_CPU_PERCENT, NULL, &curr, 100, 4);
    TEST_ASSERT_TRUE(v < 0.0f);
}

static void test_metric_mem_rss_uses_vmrss_no_prev(void)
{
    pg_raw_sample_t curr = mk_sample(7, 1, 1000, 0, 0);
    curr.vmrss_bytes = 4096;
    /* prev == NULL no impide RSS: es valor instantáneo. */
    float v = pg_alert_metric_current(PG_METRIC_MEM_RSS, NULL, &curr, 100, 4);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 4096.0f, v);
}

static void test_metric_io_read_and_write_rates(void)
{
    pg_raw_sample_t prev = mk_sample(7, 1, 0, 0, 0);
    pg_raw_sample_t curr = mk_sample(7, 1, 1000, 0, 0);
    curr.read_bytes = 1000;   /* 1000 B en 1s */
    curr.write_bytes = 2000;  /* 2000 B en 1s */
    float r = pg_alert_metric_current(PG_METRIC_IO_READ_RATE, &prev, &curr, 100, 4);
    float w = pg_alert_metric_current(PG_METRIC_IO_WRITE_RATE, &prev, &curr, 100, 4);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1000.0f, r);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2000.0f, w);
}

static void test_metric_unknown_is_sentinel(void)
{
    pg_raw_sample_t curr = mk_sample(7, 1, 1000, 0, 0);
    float v = pg_alert_metric_current(PG_METRIC_UNKNOWN, NULL, &curr, 100, 4);
    TEST_ASSERT_TRUE(v < 0.0f);
}

/* ── evaluate: state machine §5.4 ──────────────────────────────────── */

/* Política mem_rss: métrica instantánea, no requiere prev → maneja la state
 * machine sin montar deltas en el store. Umbrales en bytes. */
static const char *MEM_INI =
    "[policy:mem]\n"
    "type = performance\n"
    "metric = mem_rss\n"
    "threshold = 1000\n"
    "threshold_low = 500\n"
    "persistence = 3\n"
    "hysteresis_m = 2\n"
    "cooldown_s = 10\n"
    "actions = warn, renice:5, stop, kill\n";

static pg_alert_engine_t *engine_from(const char *ini)
{
    write_temp_ini(ini);
    pg_alert_engine_t *e = NULL;
    TEST_ASSERT_EQUAL_INT(PG_OK,
        pg_alert_engine_init(&e, g_tmp_path, TEST_PROC_BASE, 4242, 100, 4, NULL));
    return e;
}

/* Una iteración de evaluate con una muestra mem_rss=vmrss. */
static size_t eval_rss(pg_alert_engine_t *e, pg_store_t *st,
                       unsigned long long vmrss, unsigned long long now,
                       pg_alert_decision_t *out)
{
    pg_raw_sample_t s = mk_sample(7, 1, now, 0, 0);
    s.vmrss_bytes = vmrss;
    return pg_alert_evaluate(e, &s, 1, st, now, out);
}

static pg_alert_state_t *st_of(pg_alert_engine_t *e, size_t pi)
{
    pg_proc_id_t id = { .pid = 7, .starttime = 1 };
    return pg_alert_state_lookup(e->states, id, pi);
}

static void test_above_threshold_persistence_increments(void)
{
    pg_alert_engine_t *e = engine_from(MEM_INI);
    pg_store_t *st = NULL; pg_store_init(&st, 16);
    pg_alert_decision_t out[8];

    eval_rss(e, st, 2000, 1000, out);
    pg_alert_state_t *s = st_of(e, 0);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_UINT(1, s->persistence);
    eval_rss(e, st, 2000, 1500, out);
    TEST_ASSERT_EQUAL_UINT(2, s->persistence);
    TEST_ASSERT_EQUAL_UINT(0, s->hysteresis);

    pg_store_destroy(st); pg_alert_engine_destroy(e);
}

static void test_below_low_hysteresis_increments(void)
{
    pg_alert_engine_t *e = engine_from(MEM_INI);
    pg_store_t *st = NULL; pg_store_init(&st, 16);
    pg_alert_decision_t out[8];

    eval_rss(e, st, 300, 1000, out);
    pg_alert_state_t *s = st_of(e, 0);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_UINT(1, s->hysteresis);
    TEST_ASSERT_EQUAL_UINT(0, s->persistence);

    pg_store_destroy(st); pg_alert_engine_destroy(e);
}

static void test_dead_band_resets_both_counters(void)
{
    pg_alert_engine_t *e = engine_from(MEM_INI);
    pg_store_t *st = NULL; pg_store_init(&st, 16);
    pg_alert_decision_t out[8];

    eval_rss(e, st, 2000, 1000, out);
    eval_rss(e, st, 2000, 1100, out);
    pg_alert_state_t *s = st_of(e, 0);
    s->hysteresis = 5;                 /* fuerza ambos no-cero */
    eval_rss(e, st, 700, 1200, out);   /* banda muerta (500..1000) */
    TEST_ASSERT_EQUAL_UINT(0, s->persistence);
    TEST_ASSERT_EQUAL_UINT(0, s->hysteresis);

    pg_store_destroy(st); pg_alert_engine_destroy(e);
}

static void test_sentinel_freezes(void)
{
    /* cpu_percent con store vacío → sin prev → sentinel → freeze. */
    pg_alert_engine_t *e = engine_from(
        "[policy:cpu]\ntype=performance\nmetric=cpu_percent\n"
        "threshold=80\nthreshold_low=60\npersistence=2\nhysteresis_m=2\n"
        "cooldown_s=10\nactions=warn,kill\n");
    pg_store_t *st = NULL; pg_store_init(&st, 16);
    pg_alert_decision_t out[8];

    pg_raw_sample_t s = mk_sample(7, 1, 1000, 200, 0);
    size_t nd = pg_alert_evaluate(e, &s, 1, st, 1000, out);
    TEST_ASSERT_EQUAL_UINT(0, nd);
    TEST_ASSERT_NULL(st_of(e, 0));     /* freeze: no crea state */

    pg_store_destroy(st); pg_alert_engine_destroy(e);
}

static void test_absent_freezes(void)
{
    pg_alert_engine_t *e = engine_from(MEM_INI);
    pg_store_t *st = NULL; pg_store_init(&st, 16);
    pg_alert_decision_t out[8];

    eval_rss(e, st, 2000, 1000, out);
    pg_alert_state_t *s = st_of(e, 0);
    TEST_ASSERT_EQUAL_UINT(1, s->persistence);
    size_t nd = pg_alert_evaluate(e, NULL, 0, st, 2000, out);  /* sin muestras */
    TEST_ASSERT_EQUAL_UINT(0, nd);
    TEST_ASSERT_EQUAL_UINT(1, s->persistence);                 /* congelado */

    pg_store_destroy(st); pg_alert_engine_destroy(e);
}

static void test_persistence_reached_emits(void)
{
    pg_alert_engine_t *e = engine_from(MEM_INI);     /* persistence=3 */
    pg_store_t *st = NULL; pg_store_init(&st, 16);
    pg_alert_decision_t out[8];

    TEST_ASSERT_EQUAL_UINT(0, eval_rss(e, st, 2000, 1000, out));
    TEST_ASSERT_EQUAL_UINT(0, eval_rss(e, st, 2000, 1500, out));
    size_t nd = eval_rss(e, st, 2000, 2000, out);
    TEST_ASSERT_EQUAL_UINT(1, nd);
    TEST_ASSERT_EQUAL_INT(PG_ACT_WARN, out[0].kind);
    TEST_ASSERT_EQUAL_INT(0, out[0].level);
    TEST_ASSERT_EQUAL_UINT(0, out[0].policy_index);
    TEST_ASSERT_EQUAL_INT(7, out[0].id.pid);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 2000.0f, out[0].metric_value);
    TEST_ASSERT_NULL(out[0].skip_reason);
    TEST_ASSERT_EQUAL_UINT64(2000, st_of(e, 0)->alert_active_since_ms);

    pg_store_destroy(st); pg_alert_engine_destroy(e);
}

static const char *P1_INI =
    "[policy:mem]\ntype=performance\nmetric=mem_rss\n"
    "threshold=1000\nthreshold_low=500\npersistence=1\nhysteresis_m=2\n"
    "cooldown_s=10\nactions=warn,renice:5,stop,kill\n";

static void test_persistence_1_immediate(void)
{
    pg_alert_engine_t *e = engine_from(P1_INI);
    pg_store_t *st = NULL; pg_store_init(&st, 16);
    pg_alert_decision_t out[8];

    TEST_ASSERT_EQUAL_UINT(1, eval_rss(e, st, 2000, 1000, out));
    TEST_ASSERT_EQUAL_INT(PG_ACT_WARN, out[0].kind);

    pg_store_destroy(st); pg_alert_engine_destroy(e);
}

static void test_cooldown_blocks_then_expires(void)
{
    pg_alert_engine_t *e = engine_from(P1_INI);
    pg_store_t *st = NULL; pg_store_init(&st, 16);
    pg_alert_decision_t out[8];

    TEST_ASSERT_EQUAL_UINT(1, eval_rss(e, st, 2000, 1000, out));
    st_of(e, 0)->cooldown_until_ms = 5000;            /* simula act() */
    TEST_ASSERT_EQUAL_UINT(0, eval_rss(e, st, 2000, 2000, out)); /* now<cd */
    TEST_ASSERT_EQUAL_UINT(1, eval_rss(e, st, 2000, 5000, out)); /* now==cd */

    pg_store_destroy(st); pg_alert_engine_destroy(e);
}

static void test_hysteresis_deactivates_preserving_level(void)
{
    pg_alert_engine_t *e = engine_from(MEM_INI);
    pg_store_t *st = NULL; pg_store_init(&st, 16);
    pg_alert_decision_t out[8];

    eval_rss(e, st, 2000, 1000, out);
    eval_rss(e, st, 2000, 1100, out);
    eval_rss(e, st, 2000, 1200, out);                 /* activa, since=1200 */
    pg_alert_state_t *s = st_of(e, 0);
    s->escalation_level = 2;                           /* simula act() avanzó */

    eval_rss(e, st, 300, 1300, out);                   /* hysteresis 1 */
    TEST_ASSERT_EQUAL_INT(2, s->escalation_level);
    eval_rss(e, st, 300, 1400, out);                   /* hysteresis 2 → off */
    TEST_ASSERT_EQUAL_UINT(0, s->persistence);
    TEST_ASSERT_EQUAL_UINT(0, s->hysteresis);
    TEST_ASSERT_EQUAL_UINT64(0, s->alert_active_since_ms);
    TEST_ASSERT_TRUE(s->deactivated_since_last_act);
    TEST_ASSERT_EQUAL_INT(2, s->escalation_level);     /* preservado */

    pg_store_destroy(st); pg_alert_engine_destroy(e);
}

static void test_exhausted_no_new_decision(void)
{
    pg_alert_engine_t *e = engine_from(MEM_INI);       /* n_actions=4 */
    pg_store_t *st = NULL; pg_store_init(&st, 16);
    pg_alert_decision_t out[8];

    eval_rss(e, st, 2000, 1000, out);
    eval_rss(e, st, 2000, 1100, out);
    st_of(e, 0)->escalation_level = 4;                 /* agotado */
    TEST_ASSERT_EQUAL_UINT(0, eval_rss(e, st, 2000, 1200, out));

    pg_store_destroy(st); pg_alert_engine_destroy(e);
}

static void test_multiple_policies_same_proc(void)
{
    pg_alert_engine_t *e = engine_from(
        "[policy:a]\ntype=performance\nmetric=mem_rss\nthreshold=1000\n"
        "threshold_low=500\npersistence=1\nhysteresis_m=2\ncooldown_s=10\n"
        "actions=warn\n"
        "[policy:b]\ntype=performance\nmetric=mem_rss\nthreshold=1500\n"
        "threshold_low=800\npersistence=1\nhysteresis_m=2\ncooldown_s=10\n"
        "actions=warn\n");
    pg_store_t *st = NULL; pg_store_init(&st, 16);
    pg_alert_decision_t out[8];

    size_t nd = eval_rss(e, st, 2000, 1000, out);      /* > ambos umbrales */
    TEST_ASSERT_EQUAL_UINT(2, nd);

    pg_store_destroy(st); pg_alert_engine_destroy(e);
}

static void test_security_policy_skipped(void)
{
    pg_alert_engine_t *e = engine_from(
        "[policy:scan]\ntype=security\nmetric=cpu_percent\nthreshold=50\n"
        "actions=warn\n"
        "[policy:mem]\ntype=performance\nmetric=mem_rss\nthreshold=1000\n"
        "threshold_low=500\npersistence=1\nhysteresis_m=2\ncooldown_s=10\n"
        "actions=warn\n");
    pg_store_t *st = NULL; pg_store_init(&st, 16);
    pg_alert_decision_t out[8];

    size_t nd = eval_rss(e, st, 2000, 1000, out);
    TEST_ASSERT_EQUAL_UINT(1, nd);                     /* solo la perf */
    TEST_ASSERT_EQUAL_UINT(1, out[0].policy_index);    /* índice de mem */
    pg_proc_id_t id = { .pid = 7, .starttime = 1 };
    TEST_ASSERT_NULL(pg_alert_state_lookup(e->states, id, 0)); /* security sin state */

    pg_store_destroy(st); pg_alert_engine_destroy(e);
}

/* ── validate: stale_id + whitelist + cordura ──────────────────────── */

/* INI con [security] protected_names; política mem perf con stop/kill. */
static const char *VAL_INI =
    "[security]\n"
    "protected_names = sshd, systemd\n"
    "\n"
    "[policy:mem]\n"
    "type = performance\n"
    "metric = mem_rss\n"
    "threshold = 1000\n"
    "threshold_low = 500\n"
    "persistence = 1\n"
    "hysteresis_m = 2\n"
    "cooldown_s = 10\n"
    "actions = warn, renice:5, stop, kill\n";

static pg_raw_sample_t mk_full(pid_t pid, unsigned long long start, long ppid,
                               const char *comm, const char *exe)
{
    pg_raw_sample_t s = { 0 };
    s.id.pid = pid;
    s.id.starttime = start;
    s.ppid = ppid;
    snprintf(s.comm, sizeof(s.comm), "%s", comm);
    snprintf(s.exe_path, sizeof(s.exe_path), "%s", exe);
    return s;
}

static pg_alert_decision_t mk_dec(pid_t pid, unsigned long long start,
                                  pg_action_kind_t kind)
{
    pg_alert_decision_t d = { 0 };
    d.id.pid = pid;
    d.id.starttime = start;
    d.policy_index = 0;
    d.kind = kind;
    d.skip_reason = NULL;
    return d;
}

/* own_pid fijo en engine_from = 4242. */
static void test_validate_valid_passes_skip_null(void)
{
    pg_alert_engine_t *e = engine_from(VAL_INI);
    pg_raw_sample_t s = mk_full(7, 1, 999, "myapp", "/opt/app");
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_WARN);
    pg_alert_validate(e, &s, 1, &d, 1, 1000);
    TEST_ASSERT_NULL(d.skip_reason);
    pg_alert_engine_destroy(e);
}

static void test_validate_whitelist_pid_1(void)
{
    pg_alert_engine_t *e = engine_from(VAL_INI);
    pg_raw_sample_t s = mk_full(1, 1, 0, "init", "/sbin/init");
    pg_alert_decision_t d = mk_dec(1, 1, PG_ACT_KILL);
    pg_alert_validate(e, &s, 1, &d, 1, 1000);
    TEST_ASSERT_EQUAL_STRING("protected", d.skip_reason);
    pg_alert_engine_destroy(e);
}

static void test_validate_whitelist_own_pid(void)
{
    pg_alert_engine_t *e = engine_from(VAL_INI);
    pg_raw_sample_t s = mk_full(4242, 1, 999, "procguard", "/opt/procguard");
    pg_alert_decision_t d = mk_dec(4242, 1, PG_ACT_KILL);
    pg_alert_validate(e, &s, 1, &d, 1, 1000);
    TEST_ASSERT_EQUAL_STRING("protected", d.skip_reason);
    pg_alert_engine_destroy(e);
}

static void test_validate_whitelist_child_by_ppid(void)
{
    pg_alert_engine_t *e = engine_from(VAL_INI);
    pg_raw_sample_t s = mk_full(7, 1, 4242, "child", "/opt/child");
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_KILL);
    pg_alert_validate(e, &s, 1, &d, 1, 1000);
    TEST_ASSERT_EQUAL_STRING("protected", d.skip_reason);
    pg_alert_engine_destroy(e);
}

static void test_validate_whitelist_protected_std_path(void)
{
    pg_alert_engine_t *e = engine_from(VAL_INI);
    pg_raw_sample_t s = mk_full(7, 1, 999, "sshd", "/usr/sbin/sshd");
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_KILL);
    pg_alert_validate(e, &s, 1, &d, 1, 1000);
    TEST_ASSERT_EQUAL_STRING("protected", d.skip_reason);
    pg_alert_engine_destroy(e);
}

static void test_validate_protected_name_nonstd_path_not_protected(void)
{
    /* sshd pero desde /tmp → NO protegido (disguised). WARN no gatilla cordura. */
    pg_alert_engine_t *e = engine_from(VAL_INI);
    pg_raw_sample_t s = mk_full(7, 1, 999, "sshd", "/tmp/sshd");
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_WARN);
    pg_alert_validate(e, &s, 1, &d, 1, 1000);
    TEST_ASSERT_NULL(d.skip_reason);
    pg_alert_engine_destroy(e);
}

static void test_validate_kernel_thread_empty_exe(void)
{
    pg_alert_engine_t *e = engine_from(VAL_INI);
    pg_raw_sample_t s = mk_full(7, 1, 2, "kworker", "");
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_KILL);
    pg_alert_validate(e, &s, 1, &d, 1, 1000);
    TEST_ASSERT_EQUAL_STRING("protected", d.skip_reason);
    pg_alert_engine_destroy(e);
}

static void test_validate_stale_starttime_skipped(void)
{
    pg_alert_engine_t *e = engine_from(VAL_INI);
    pg_raw_sample_t s = mk_full(7, 2, 999, "app", "/opt/app");  /* start=2 */
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_KILL);          /* start=1 */
    pg_alert_validate(e, &s, 1, &d, 1, 1000);
    TEST_ASSERT_EQUAL_STRING("stale_id", d.skip_reason);
    pg_alert_engine_destroy(e);
}

static void test_validate_sanity_5s_blocks_kill(void)
{
    pg_alert_engine_t *e = engine_from(VAL_INI);
    pg_proc_id_t id = { .pid = 7, .starttime = 1 };
    pg_alert_state_t *st = NULL;
    pg_alert_state_upsert(e->states, id, 0, &st);
    st->alert_active_since_ms = 1000;                  /* activa hace 2s */
    pg_raw_sample_t s = mk_full(7, 1, 999, "app", "/opt/app");
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_KILL);
    pg_alert_validate(e, &s, 1, &d, 1, 3000);          /* 3000-1000=2000<5000 */
    TEST_ASSERT_EQUAL_STRING("sanity", d.skip_reason);
    pg_alert_engine_destroy(e);
}

static void test_validate_sanity_ok_after_5s(void)
{
    pg_alert_engine_t *e = engine_from(VAL_INI);
    pg_proc_id_t id = { .pid = 7, .starttime = 1 };
    pg_alert_state_t *st = NULL;
    pg_alert_state_upsert(e->states, id, 0, &st);
    st->alert_active_since_ms = 1000;
    pg_raw_sample_t s = mk_full(7, 1, 999, "app", "/opt/app");
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_KILL);
    pg_alert_validate(e, &s, 1, &d, 1, 7000);          /* 7000-1000=6000>=5000 */
    TEST_ASSERT_NULL(d.skip_reason);
    pg_alert_engine_destroy(e);
}

/* ── act: dispatch + TOCTOU + techo kills/min (Fase 6) ─────────────── */

#define ACT_PROC_BASE "/tmp/pg_test_act_proc"

/* Syscalls grabadores inyectados (ADR-009): cuentan invocaciones y argumentos. */
static int   g_kill_calls;
static int   g_last_kill_sig;
static pid_t g_last_kill_pid;
static int   g_setprio_calls;
static int   g_last_setprio_val;

static int rec_kill(pid_t pid, int sig)
{
    g_kill_calls++;
    g_last_kill_pid = pid;
    g_last_kill_sig = sig;
    return 0;
}
static int rec_setpriority(int which, id_t who, int prio)
{
    (void)which; (void)who;
    g_setprio_calls++;
    g_last_setprio_val = prio;
    return 0;
}

/* Stub del backend de cage (ADR-018): graba llamadas; retorno controlable. */
static int      g_cage_apply_calls;
static pid_t    g_last_cage_pid;
static unsigned g_last_cage_pct;
static int      g_cage_apply_ret;       /* lo que devuelve rec_cage_apply */
static int      g_cage_release_calls;
static pid_t    g_last_release_pid;

static int rec_cage_apply(pid_t pid, unsigned pct)
{
    g_cage_apply_calls++;
    g_last_cage_pid = pid;
    g_last_cage_pct = pct;
    return g_cage_apply_ret;
}
static int rec_cage_release(pid_t pid)
{
    g_cage_release_calls++;
    g_last_release_pid = pid;
    return PG_OK;
}

static void act_reset(void)
{
    g_kill_calls = 0; g_last_kill_sig = 0; g_last_kill_pid = 0;
    g_setprio_calls = 0; g_last_setprio_val = 0;
    g_cage_apply_calls = 0; g_last_cage_pid = 0; g_last_cage_pct = 0;
    g_cage_apply_ret = PG_OK;
    g_cage_release_calls = 0; g_last_release_pid = 0;
}

/* Crea ACT_PROC_BASE/<pid>/stat con `start` en el campo 22 (guard TOCTOU). */
static void act_write_stat(pid_t pid, unsigned long long start)
{
    char dir[128], path[160], content[256];
    mkdir(ACT_PROC_BASE, 0755);
    snprintf(dir, sizeof(dir), ACT_PROC_BASE "/%d", pid);
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/stat", dir);
    snprintf(content, sizeof(content),
        "%d (proc) S 1 %d %d 0 -1 0 0 0 0 0 10 5 0 0 20 0 1 0 %llu 0 0\n",
        pid, pid, pid, start);
    FILE *f = fopen(path, "w");
    if (f != NULL) { fputs(content, f); fclose(f); }
}

/* Engine con proc_base sintético + syscalls grabadores. */
static pg_alert_engine_t *act_engine(const char *ini)
{
    write_temp_ini(ini);
    pg_syscalls_t sc = { .kill = rec_kill, .setpriority = rec_setpriority };
    pg_alert_engine_t *e = NULL;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_alert_engine_init(
        &e, g_tmp_path, ACT_PROC_BASE, 4242, 100, 4, &sc));
    act_reset();
    return e;
}

/* State para (pid=7, start=1, policy 0): act lo busca para mutarlo. */
static pg_alert_state_t *act_state(pg_alert_engine_t *e)
{
    pg_proc_id_t id = { .pid = 7, .starttime = 1 };
    pg_alert_state_t *st = NULL;
    pg_alert_state_upsert(e->states, id, 0, &st);
    return st;
}

/* INI de act: una política mem con la secuencia completa; dry_run desactivado
 * para ejercitar el dispatch real (cooldown_s=10 → +10000 ms). */
static const char *ACT_INI =
    "[global]\n"
    "dry_run = false\n"
    "[policy:mem]\n"
    "type = performance\n"
    "metric = mem_rss\n"
    "threshold = 1000\n"
    "threshold_low = 500\n"
    "persistence = 1\n"
    "hysteresis_m = 2\n"
    "cooldown_s = 10\n"
    "actions = warn, renice:10, stop, kill\n";

static void test_act_warn_no_syscall_advances_level(void)
{
    pg_alert_engine_t *e = act_engine(ACT_INI);
    pg_alert_state_t *st = act_state(e);
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_WARN);

    pg_alert_act(e, &d, 1, 1000);
    TEST_ASSERT_EQUAL_INT(0, g_kill_calls);
    TEST_ASSERT_EQUAL_INT(0, g_setprio_calls);
    TEST_ASSERT_EQUAL_INT(1, st->escalation_level);          /* avanzó 0→1 */
    TEST_ASSERT_EQUAL_UINT64(11000, st->cooldown_until_ms);  /* now+10s */
    pg_alert_engine_destroy(e);
}

static void test_act_renice_calls_setpriority(void)
{
    pg_alert_engine_t *e = act_engine(ACT_INI);
    pg_alert_state_t *st = act_state(e);
    act_write_stat(7, 1);                          /* TOCTOU OK */
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_RENICE);
    d.param = 10;

    pg_alert_act(e, &d, 1, 1000);
    TEST_ASSERT_EQUAL_INT(1, g_setprio_calls);
    TEST_ASSERT_EQUAL_INT(10, g_last_setprio_val);
    TEST_ASSERT_EQUAL_INT(0, g_kill_calls);
    TEST_ASSERT_EQUAL_INT(1, st->escalation_level);
    pg_alert_engine_destroy(e);
}

static void test_act_stop_calls_kill_sigstop(void)
{
    pg_alert_engine_t *e = act_engine(ACT_INI);
    pg_alert_state_t *st = act_state(e);
    act_write_stat(7, 1);
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_STOP);

    pg_alert_act(e, &d, 1, 1000);
    TEST_ASSERT_EQUAL_INT(1, g_kill_calls);
    TEST_ASSERT_EQUAL_INT(SIGSTOP, g_last_kill_sig);
    TEST_ASSERT_EQUAL_INT(7, g_last_kill_pid);
    TEST_ASSERT_EQUAL_INT(1, st->escalation_level);
    pg_alert_engine_destroy(e);
}

static void test_act_kill_sigkill_and_pushes_ring(void)
{
    pg_alert_engine_t *e = act_engine(ACT_INI);
    pg_alert_state_t *st = act_state(e);
    act_write_stat(7, 1);
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_KILL);

    pg_alert_act(e, &d, 1, 1000);
    TEST_ASSERT_EQUAL_INT(1, g_kill_calls);
    TEST_ASSERT_EQUAL_INT(SIGKILL, g_last_kill_sig);
    TEST_ASSERT_EQUAL_UINT(1, e->kill_ring_count);      /* push tras KILL */
    TEST_ASSERT_EQUAL_UINT64(1000, e->kill_ring[0]);
    TEST_ASSERT_EQUAL_INT(1, st->escalation_level);
    pg_alert_engine_destroy(e);
}

static void test_act_cage_not_impl_no_syscall_advances(void)
{
    /* CAGE no implementada: no syscall, pero AVANZA nivel (no-stall ADR-014). */
    pg_alert_engine_t *e = act_engine(ACT_INI);
    pg_alert_state_t *st = act_state(e);
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_CAGE);

    pg_alert_act(e, &d, 1, 1000);
    TEST_ASSERT_EQUAL_INT(0, g_kill_calls);
    TEST_ASSERT_EQUAL_INT(0, g_setprio_calls);
    TEST_ASSERT_EQUAL_INT(1, st->escalation_level);        /* avanza igual */
    TEST_ASSERT_EQUAL_UINT64(11000, st->cooldown_until_ms);
    pg_alert_engine_destroy(e);
}

static void test_act_skip_reason_no_dispatch_no_advance(void)
{
    pg_alert_engine_t *e = act_engine(ACT_INI);
    pg_alert_state_t *st = act_state(e);
    act_write_stat(7, 1);
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_KILL);
    d.skip_reason = "protected";                  /* validate lo marcó */

    pg_alert_act(e, &d, 1, 1000);
    TEST_ASSERT_EQUAL_INT(0, g_kill_calls);
    TEST_ASSERT_EQUAL_INT(0, st->escalation_level);        /* no avanza */
    TEST_ASSERT_EQUAL_UINT64(0, st->cooldown_until_ms);
    pg_alert_engine_destroy(e);
}

static void test_act_reactivation_same_level(void)
{
    /* deactivated_since_last_act=true → reactivación: mismo nivel, flag a false. */
    pg_alert_engine_t *e = act_engine(ACT_INI);
    pg_alert_state_t *st = act_state(e);
    st->escalation_level = 2;
    st->deactivated_since_last_act = true;
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_WARN);

    pg_alert_act(e, &d, 1, 1000);
    TEST_ASSERT_EQUAL_INT(2, st->escalation_level);        /* sin avanzar */
    TEST_ASSERT_FALSE(st->deactivated_since_last_act);     /* consumido */
    TEST_ASSERT_EQUAL_UINT64(11000, st->cooldown_until_ms);
    pg_alert_engine_destroy(e);
}

/* dry_run=true: previsualiza la secuencia sin tocar procesos. */
static const char *ACT_DRY_INI =
    "[global]\n"
    "dry_run = true\n"
    "[policy:mem]\n"
    "type = performance\n"
    "metric = mem_rss\n"
    "threshold = 1000\n"
    "threshold_low = 500\n"
    "persistence = 1\n"
    "hysteresis_m = 2\n"
    "cooldown_s = 10\n"
    "actions = warn, renice:10, stop, kill\n";

static void test_act_toctou_mismatch_cancels(void)
{
    /* starttime real (999) ≠ id.starttime de la decisión (1) → PID reciclado. */
    pg_alert_engine_t *e = act_engine(ACT_INI);
    pg_alert_state_t *st = act_state(e);
    act_write_stat(7, 999);
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_KILL);

    pg_alert_act(e, &d, 1, 1000);
    TEST_ASSERT_EQUAL_INT(0, g_kill_calls);                 /* cancelado */
    TEST_ASSERT_EQUAL_INT(0, st->escalation_level);         /* no avanza */
    TEST_ASSERT_EQUAL_UINT64(0, st->cooldown_until_ms);
    pg_alert_engine_destroy(e);
}

static void test_act_toctou_process_gone_cancels(void)
{
    /* Sin stat para pid 7 → read_starttime PG_ERR_IO → proceso desaparecido. */
    pg_alert_engine_t *e = act_engine(ACT_INI);
    pg_alert_state_t *st = act_state(e);
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_STOP);

    pg_alert_act(e, &d, 1, 1000);
    TEST_ASSERT_EQUAL_INT(0, g_kill_calls);
    TEST_ASSERT_EQUAL_INT(0, st->escalation_level);
    pg_alert_engine_destroy(e);
}

static void test_act_dry_run_suppresses_syscalls_advances_level(void)
{
    /* Sin stat: el dry-run NO hace TOCTOU (no hay syscall real que proteger). */
    pg_alert_engine_t *e = act_engine(ACT_DRY_INI);
    pg_alert_state_t *st = act_state(e);
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_KILL);

    pg_alert_act(e, &d, 1, 1000);
    TEST_ASSERT_EQUAL_INT(0, g_kill_calls);                 /* no ejecuta */
    TEST_ASSERT_EQUAL_UINT(0, e->kill_ring_count);          /* no push */
    TEST_ASSERT_EQUAL_INT(1, st->escalation_level);         /* sí avanza */
    TEST_ASSERT_EQUAL_UINT64(11000, st->cooldown_until_ms);
    pg_alert_engine_destroy(e);
}

static void test_act_ceiling_blocks_kill_no_advance(void)
{
    /* Ring lleno (3 kills recientes) → KILL bloqueado, transitorio: no avanza. */
    pg_alert_engine_t *e = act_engine(ACT_INI);     /* max_kills_per_minute=3 */
    pg_alert_state_t *st = act_state(e);
    act_write_stat(7, 1);
    e->kill_ring_count = 3;
    e->kill_ring_head = 0;
    e->kill_ring[0] = e->kill_ring[1] = e->kill_ring[2] = 1000;
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_KILL);

    pg_alert_act(e, &d, 1, 1000);
    TEST_ASSERT_EQUAL_INT(0, g_kill_calls);                 /* ceiling */
    TEST_ASSERT_EQUAL_UINT(3, e->kill_ring_count);          /* no push extra */
    TEST_ASSERT_EQUAL_INT(0, st->escalation_level);         /* no avanza */
    TEST_ASSERT_EQUAL_UINT64(0, st->cooldown_until_ms);     /* reintenta luego */
    pg_alert_engine_destroy(e);
}

/* ── act: cage real (Slice 4c, ADR-018) ────────────────────────────── */

/* Engine con backend de cage inyectado (grabador) + syscalls grabadores. */
static pg_alert_engine_t *cage_engine(const char *ini)
{
    write_temp_ini(ini);
    pg_syscalls_t sc = { .kill = rec_kill, .setpriority = rec_setpriority,
                         .cage_apply = rec_cage_apply,
                         .cage_release = rec_cage_release };
    pg_alert_engine_t *e = NULL;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_alert_engine_init(
        &e, g_tmp_path, ACT_PROC_BASE, 4242, 100, 4, &sc));
    act_reset();
    return e;
}

/* max_caged_processes=2 (techo testeable), cage_cpu_percent=40. */
static const char *CAGE_INI =
    "[global]\n"
    "dry_run = false\n"
    "max_caged_processes = 2\n"
    "cage_cpu_percent = 40\n"
    "[policy:mem]\n"
    "type = performance\n"
    "metric = mem_rss\n"
    "threshold = 1000\n"
    "threshold_low = 500\n"
    "persistence = 1\n"
    "hysteresis_m = 2\n"
    "cooldown_s = 10\n"
    "actions = warn, cage, stop, kill\n";

static void test_act_cage_applies_and_counts(void)
{
    pg_alert_engine_t *e = cage_engine(CAGE_INI);
    pg_alert_state_t *st = act_state(e);
    act_write_stat(7, 1);                              /* TOCTOU OK */
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_CAGE);

    pg_alert_act(e, &d, 1, 1000);
    TEST_ASSERT_EQUAL_INT(1, g_cage_apply_calls);
    TEST_ASSERT_EQUAL_INT(7, g_last_cage_pid);
    TEST_ASSERT_EQUAL_UINT(40, g_last_cage_pct);       /* cage_cpu_percent */
    TEST_ASSERT_EQUAL_UINT(1, e->caged_count);
    TEST_ASSERT_EQUAL_INT(1, st->escalation_level);
    TEST_ASSERT_EQUAL_UINT64(11000, st->cooldown_until_ms);
    pg_alert_engine_destroy(e);
}

static void test_act_cage_idempotent_no_double_count(void)
{
    pg_alert_engine_t *e = cage_engine(CAGE_INI);
    pg_alert_state_t *st = act_state(e);
    act_write_stat(7, 1);
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_CAGE);

    pg_alert_act(e, &d, 1, 1000);
    pg_alert_act(e, &d, 1, 2000);                      /* re-cage mismo pid */
    TEST_ASSERT_EQUAL_INT(2, g_cage_apply_calls);      /* re-aplica idempotente */
    TEST_ASSERT_EQUAL_UINT(1, e->caged_count);         /* sin doble conteo */
    TEST_ASSERT_EQUAL_INT(2, st->escalation_level);    /* avanza ambas veces */
    pg_alert_engine_destroy(e);
}

static void test_act_cage_ceiling_no_stall_advances(void)
{
    /* Cage lleno (2 pids distintos) → cage de pid nuevo no aplica pero avanza. */
    pg_alert_engine_t *e = cage_engine(CAGE_INI);
    pg_alert_state_t *st = act_state(e);
    act_write_stat(7, 1);
    e->caged[0] = (pg_proc_id_t){ .pid = 8, .starttime = 1 };
    e->caged[1] = (pg_proc_id_t){ .pid = 9, .starttime = 1 };
    e->caged_count = 2;                                /* == max_caged_processes */
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_CAGE);

    pg_alert_act(e, &d, 1, 1000);
    TEST_ASSERT_EQUAL_INT(0, g_cage_apply_calls);      /* no aplica (techo) */
    TEST_ASSERT_EQUAL_UINT(2, e->caged_count);
    TEST_ASSERT_EQUAL_INT(1, st->escalation_level);    /* no-stall: avanza */
    TEST_ASSERT_EQUAL_UINT64(11000, st->cooldown_until_ms);
    pg_alert_engine_destroy(e);
}

static void test_act_cage_toctou_mismatch_cancels(void)
{
    pg_alert_engine_t *e = cage_engine(CAGE_INI);
    pg_alert_state_t *st = act_state(e);
    act_write_stat(7, 999);                            /* starttime ≠ decisión */
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_CAGE);

    pg_alert_act(e, &d, 1, 1000);
    TEST_ASSERT_EQUAL_INT(0, g_cage_apply_calls);      /* cancelado */
    TEST_ASSERT_EQUAL_UINT(0, e->caged_count);
    TEST_ASSERT_EQUAL_INT(0, st->escalation_level);    /* no avanza */
    pg_alert_engine_destroy(e);
}

static void test_act_cage_backend_failure_no_stall(void)
{
    pg_alert_engine_t *e = cage_engine(CAGE_INI);
    pg_alert_state_t *st = act_state(e);
    act_write_stat(7, 1);
    g_cage_apply_ret = PG_ERR_IO;                      /* backend falla */
    pg_alert_decision_t d = mk_dec(7, 1, PG_ACT_CAGE);

    pg_alert_act(e, &d, 1, 1000);
    TEST_ASSERT_EQUAL_INT(1, g_cage_apply_calls);      /* lo intentó */
    TEST_ASSERT_EQUAL_UINT(0, e->caged_count);         /* no se contabiliza */
    TEST_ASSERT_EQUAL_INT(1, st->escalation_level);    /* no-stall: avanza */
    pg_alert_engine_destroy(e);
}

/* ── cycle + gc: ensamblaje del pipeline (ADR-010/013) ─────────────── */

static void test_cycle_runs_full_pipeline_dry_run(void)
{
    pg_alert_engine_t *e = act_engine(ACT_DRY_INI);   /* dry_run, persistence=1 */
    pg_store_t *st = NULL; pg_store_init(&st, 16);
    pg_raw_sample_t s = mk_full(7, 1, 999, "app", "/opt/app");
    s.vmrss_bytes = 2000;                              /* > threshold 1000 */

    int rc = pg_alert_engine_cycle(e, &s, 1, st, 1000);
    TEST_ASSERT_EQUAL_INT(PG_OK, rc);
    pg_proc_id_t id = { .pid = 7, .starttime = 1 };
    pg_alert_state_t *state = pg_alert_state_lookup(e->states, id, 0);
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_EQUAL_INT(1, state->escalation_level);  /* act (dry-run) avanzó */
    TEST_ASSERT_EQUAL_INT(0, g_kill_calls);             /* dry-run no ejecuta */

    pg_store_destroy(st); pg_alert_engine_destroy(e);
}

static void test_cycle_null_engine_returns_parse_err(void)
{
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE,
        pg_alert_engine_cycle(NULL, NULL, 0, NULL, 0));
}

static void test_engine_gc_releases_absent_state(void)
{
    pg_alert_engine_t *e = act_engine(ACT_DRY_INI);
    pg_store_t *st = NULL; pg_store_init(&st, 16);
    pg_proc_id_t id = { .pid = 7, .starttime = 1 };
    pg_alert_state_t *s = NULL;
    pg_alert_state_upsert(e->states, id, 0, &s);
    TEST_ASSERT_NOT_NULL(pg_alert_state_lookup(e->states, id, 0));

    pg_alert_engine_gc(e, st);                          /* pid 7 ausente del store */
    TEST_ASSERT_NULL(pg_alert_state_lookup(e->states, id, 0));

    pg_store_destroy(st); pg_alert_engine_destroy(e);
}

static void test_gc_releases_cage_when_absent(void)
{
    pg_alert_engine_t *e = cage_engine(CAGE_INI);
    pg_store_t *st = NULL; pg_store_init(&st, 16);
    e->caged[0] = (pg_proc_id_t){ .pid = 7, .starttime = 1 };
    e->caged_count = 1;                                /* pid 7 ausente del store */

    pg_alert_engine_gc(e, st);
    TEST_ASSERT_EQUAL_INT(1, g_cage_release_calls);
    TEST_ASSERT_EQUAL_INT(7, g_last_release_pid);
    TEST_ASSERT_EQUAL_UINT(0, e->caged_count);

    pg_store_destroy(st); pg_alert_engine_destroy(e);
}

static void test_gc_keeps_cage_present(void)
{
    pg_alert_engine_t *e = cage_engine(CAGE_INI);
    pg_store_t *st = NULL; pg_store_init(&st, 16);
    pg_raw_sample_t s = mk_sample(7, 1, 1000, 0, 0);
    pg_store_insert(st, &s);                           /* pid 7 presente */
    e->caged[0] = (pg_proc_id_t){ .pid = 7, .starttime = 1 };
    e->caged_count = 1;

    pg_alert_engine_gc(e, st);
    TEST_ASSERT_EQUAL_INT(0, g_cage_release_calls);    /* presente → no libera */
    TEST_ASSERT_EQUAL_UINT(1, e->caged_count);

    pg_store_destroy(st); pg_alert_engine_destroy(e);
}

static void test_engine_global_exposes_config(void)
{
    pg_alert_engine_t *e = engine_from(
        "[global]\nsample_interval=250\nsample_buffer=8\ndry_run=false\n"
        "[policy:mem]\ntype=performance\nmetric=mem_rss\nthreshold=1000\n"
        "threshold_low=500\npersistence=1\nhysteresis_m=2\ncooldown_s=10\n"
        "actions=warn\n");
    const pg_global_config_t *g = pg_alert_engine_global(e);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT(250, g->sample_interval_ms);
    TEST_ASSERT_EQUAL_UINT(8, g->sample_buffer);
    TEST_ASSERT_FALSE(g->dry_run);
    pg_alert_engine_destroy(e);
}

static void test_engine_global_null_safe(void)
{
    TEST_ASSERT_NULL(pg_alert_engine_global(NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_loads_policies_and_defaults);
    RUN_TEST(test_init_null_args_return_parse_err);
    RUN_TEST(test_init_unreadable_ini_returns_io_err);
    RUN_TEST(test_init_invalid_ini_returns_parse_err);
    RUN_TEST(test_init_stores_injected_syscalls);
    RUN_TEST(test_init_copies_proc_base_and_allocs_ring);
    RUN_TEST(test_default_syscalls_wires_cage);
    RUN_TEST(test_init_null_proc_base_returns_parse_err);
    RUN_TEST(test_metric_cpu_percent_maps_to_m3);
    RUN_TEST(test_metric_cpu_percent_null_prev_is_sentinel);
    RUN_TEST(test_metric_mem_rss_uses_vmrss_no_prev);
    RUN_TEST(test_metric_io_read_and_write_rates);
    RUN_TEST(test_metric_unknown_is_sentinel);
    RUN_TEST(test_above_threshold_persistence_increments);
    RUN_TEST(test_below_low_hysteresis_increments);
    RUN_TEST(test_dead_band_resets_both_counters);
    RUN_TEST(test_sentinel_freezes);
    RUN_TEST(test_absent_freezes);
    RUN_TEST(test_persistence_reached_emits);
    RUN_TEST(test_persistence_1_immediate);
    RUN_TEST(test_cooldown_blocks_then_expires);
    RUN_TEST(test_hysteresis_deactivates_preserving_level);
    RUN_TEST(test_exhausted_no_new_decision);
    RUN_TEST(test_multiple_policies_same_proc);
    RUN_TEST(test_security_policy_skipped);
    RUN_TEST(test_validate_valid_passes_skip_null);
    RUN_TEST(test_validate_whitelist_pid_1);
    RUN_TEST(test_validate_whitelist_own_pid);
    RUN_TEST(test_validate_whitelist_child_by_ppid);
    RUN_TEST(test_validate_whitelist_protected_std_path);
    RUN_TEST(test_validate_protected_name_nonstd_path_not_protected);
    RUN_TEST(test_validate_kernel_thread_empty_exe);
    RUN_TEST(test_validate_stale_starttime_skipped);
    RUN_TEST(test_validate_sanity_5s_blocks_kill);
    RUN_TEST(test_validate_sanity_ok_after_5s);
    RUN_TEST(test_act_warn_no_syscall_advances_level);
    RUN_TEST(test_act_renice_calls_setpriority);
    RUN_TEST(test_act_stop_calls_kill_sigstop);
    RUN_TEST(test_act_kill_sigkill_and_pushes_ring);
    RUN_TEST(test_act_cage_not_impl_no_syscall_advances);
    RUN_TEST(test_act_skip_reason_no_dispatch_no_advance);
    RUN_TEST(test_act_reactivation_same_level);
    RUN_TEST(test_act_toctou_mismatch_cancels);
    RUN_TEST(test_act_toctou_process_gone_cancels);
    RUN_TEST(test_act_dry_run_suppresses_syscalls_advances_level);
    RUN_TEST(test_act_ceiling_blocks_kill_no_advance);
    RUN_TEST(test_act_cage_applies_and_counts);
    RUN_TEST(test_act_cage_idempotent_no_double_count);
    RUN_TEST(test_act_cage_ceiling_no_stall_advances);
    RUN_TEST(test_act_cage_toctou_mismatch_cancels);
    RUN_TEST(test_act_cage_backend_failure_no_stall);
    RUN_TEST(test_cycle_runs_full_pipeline_dry_run);
    RUN_TEST(test_cycle_null_engine_returns_parse_err);
    RUN_TEST(test_engine_gc_releases_absent_state);
    RUN_TEST(test_gc_releases_cage_when_absent);
    RUN_TEST(test_gc_keeps_cage_present);
    RUN_TEST(test_engine_global_exposes_config);
    RUN_TEST(test_engine_global_null_safe);
    return UNITY_END();
}
