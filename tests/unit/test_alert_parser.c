#include "unity.h"
#include "alert_policy.h"
#include "alert_config.h"
#include "pg_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Cada test escribe su INI en un path único (mkstemp) y lo unlinkea en
 * tearDown. Cualquier test con fallo entre medio deja el archivo para
 * post-mortem en /tmp. */
static char g_tmp_path[64];

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
}

static void write_temp_ini(const char *contents)
{
    strcpy(g_tmp_path, "/tmp/pg_test_alert_XXXXXX");
    int fd = mkstemp(g_tmp_path);
    TEST_ASSERT_TRUE_MESSAGE(fd >= 0, "mkstemp failed");
    size_t len = strlen(contents);
    ssize_t n = write(fd, contents, len);
    TEST_ASSERT_EQUAL_INT((int)len, (int)n);
    close(fd);
}

/* ── 1. Happy path: INI completo se parsea con todos los campos. ──── */
static void test_happy_ini_parses_all_fields(void)
{
    write_temp_ini(
        "[global]\n"
        "sample_interval = 500\n"
        "sample_buffer = 120\n"
        "max_kills_per_minute = 3\n"
        "max_caged_processes = 10\n"
        "dry_run = true\n"
        "\n"
        "[policy:cpu_hog]\n"
        "type = performance\n"
        "risk = high\n"
        "metric = cpu_percent\n"
        "threshold = 80.0\n"
        "threshold_low = 60.0\n"
        "persistence = 3\n"
        "hysteresis_m = 2\n"
        "cooldown_s = 10\n"
        "actions = warn, renice:10, stop, kill\n");

    pg_policy_t *pols = NULL;
    size_t n_pols = 0;
    pg_global_config_t g = { 0 };
    pg_security_config_t sec = { 0 };
    size_t n_errs = 99;

    int rc = pg_policy_catalog_load(g_tmp_path, &pols, &n_pols, &g, &sec, &n_errs);
    TEST_ASSERT_EQUAL_INT(PG_OK, rc);
    TEST_ASSERT_EQUAL_UINT(0, n_errs);
    TEST_ASSERT_EQUAL_UINT(1, n_pols);

    TEST_ASSERT_EQUAL_STRING("cpu_hog", pols[0].name);
    TEST_ASSERT_EQUAL_INT(PG_POLICY_PERF, pols[0].type);
    TEST_ASSERT_EQUAL_INT(PG_RISK_HIGH, pols[0].risk);
    TEST_ASSERT_EQUAL_INT(PG_METRIC_CPU_PERCENT, pols[0].metric);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 80.0f, pols[0].threshold);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 60.0f, pols[0].threshold_low);
    TEST_ASSERT_EQUAL_UINT(3, pols[0].persistence);
    TEST_ASSERT_EQUAL_UINT(2, pols[0].hysteresis_m);
    TEST_ASSERT_EQUAL_UINT(10, pols[0].cooldown_s);
    TEST_ASSERT_EQUAL_UINT(4, pols[0].n_actions);
    TEST_ASSERT_EQUAL_INT(PG_ACT_WARN,   pols[0].actions[0].kind);
    TEST_ASSERT_EQUAL_INT(PG_ACT_RENICE, pols[0].actions[1].kind);
    TEST_ASSERT_EQUAL_INT(10,            pols[0].actions[1].param);
    TEST_ASSERT_EQUAL_INT(PG_ACT_STOP,   pols[0].actions[2].kind);
    TEST_ASSERT_EQUAL_INT(PG_ACT_KILL,   pols[0].actions[3].kind);

    TEST_ASSERT_EQUAL_UINT(500, g.sample_interval_ms);
    TEST_ASSERT_EQUAL_UINT(120, g.sample_buffer);
    TEST_ASSERT_EQUAL_UINT(3,   g.max_kills_per_minute);
    TEST_ASSERT_EQUAL_UINT(10,  g.max_caged_processes);
    TEST_ASSERT_TRUE(g.dry_run);

    pg_policy_catalog_destroy(pols, n_pols);
    pg_security_config_destroy(&sec);
}

/* ── 2. Policy sin `actions` → error (ADR-011). ────────────────────── */
static void test_missing_actions_rejected(void)
{
    write_temp_ini(
        "[policy:foo]\n"
        "metric = cpu_percent\n"
        "threshold = 80.0\n");

    pg_policy_t *pols = NULL;
    size_t n_pols = 0;
    pg_global_config_t g = { 0 };
    pg_security_config_t sec = { 0 };
    size_t n_errs = 0;

    int rc = pg_policy_catalog_load(g_tmp_path, &pols, &n_pols, &g, &sec, &n_errs);
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, rc);
    TEST_ASSERT_TRUE_MESSAGE(n_errs >= 1, "expected at least one error");
}

/* ── 3. Métrica desconocida → error. ───────────────────────────────── */
static void test_unknown_metric_rejected(void)
{
    write_temp_ini(
        "[policy:foo]\n"
        "metric = disk_read\n"
        "threshold = 80.0\n"
        "actions = warn\n");

    pg_policy_t *pols = NULL;
    size_t n_pols = 0;
    pg_global_config_t g = { 0 };
    pg_security_config_t sec = { 0 };
    size_t n_errs = 0;

    int rc = pg_policy_catalog_load(g_tmp_path, &pols, &n_pols, &g, &sec, &n_errs);
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, rc);
    TEST_ASSERT_TRUE(n_errs >= 1);
}

/* ── 4. threshold_low >= threshold → error. ────────────────────────── */
static void test_threshold_inversion_rejected(void)
{
    write_temp_ini(
        "[policy:foo]\n"
        "metric = cpu_percent\n"
        "threshold = 30.0\n"
        "threshold_low = 70.0\n"
        "actions = warn\n");

    pg_policy_t *pols = NULL;
    size_t n_pols = 0;
    pg_global_config_t g = { 0 };
    pg_security_config_t sec = { 0 };
    size_t n_errs = 0;

    int rc = pg_policy_catalog_load(g_tmp_path, &pols, &n_pols, &g, &sec, &n_errs);
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, rc);
    TEST_ASSERT_TRUE(n_errs >= 1);
}

/* ── 5. [security] + type=security: ambos conviven sin error. ─────── */
static void test_security_section_accepted(void)
{
    write_temp_ini(
        "[security]\n"
        "protected_names = init, sshd\n"
        "\n"
        "[policy:suspicious_scan]\n"
        "type = security\n"
        "metric = cpu_percent\n"
        "threshold = 50.0\n"
        "actions = warn\n");

    pg_policy_t *pols = NULL;
    size_t n_pols = 0;
    pg_global_config_t g = { 0 };
    pg_security_config_t sec = { 0 };
    size_t n_errs = 99;

    int rc = pg_policy_catalog_load(g_tmp_path, &pols, &n_pols, &g, &sec, &n_errs);
    TEST_ASSERT_EQUAL_INT(PG_OK, rc);
    TEST_ASSERT_EQUAL_UINT(0, n_errs);

    TEST_ASSERT_EQUAL_UINT(2, sec.n_protected_names);
    TEST_ASSERT_EQUAL_STRING("init", sec.protected_names[0]);
    TEST_ASSERT_EQUAL_STRING("sshd", sec.protected_names[1]);

    TEST_ASSERT_EQUAL_UINT(1, n_pols);
    TEST_ASSERT_EQUAL_INT(PG_POLICY_SECURITY, pols[0].type);

    pg_policy_catalog_destroy(pols, n_pols);
    pg_security_config_destroy(&sec);
}

/* ── 6. [global] ausente → defaults del PDF §5.4. ──────────────────── */
static void test_global_defaults(void)
{
    /* Policy mínima válida para que el catálogo no quede vacío. */
    write_temp_ini(
        "[policy:minimal]\n"
        "metric = cpu_percent\n"
        "threshold = 50.0\n"
        "actions = warn\n");

    pg_policy_t *pols = NULL;
    size_t n_pols = 0;
    pg_global_config_t g = { 0 };
    pg_security_config_t sec = { 0 };
    size_t n_errs = 99;

    int rc = pg_policy_catalog_load(g_tmp_path, &pols, &n_pols, &g, &sec, &n_errs);
    TEST_ASSERT_EQUAL_INT(PG_OK, rc);
    TEST_ASSERT_EQUAL_UINT(0, n_errs);

    TEST_ASSERT_EQUAL_UINT(500, g.sample_interval_ms);
    TEST_ASSERT_EQUAL_UINT(120, g.sample_buffer);
    TEST_ASSERT_EQUAL_UINT(3,   g.max_kills_per_minute);
    TEST_ASSERT_EQUAL_UINT(10,  g.max_caged_processes);
    TEST_ASSERT_TRUE(g.dry_run);

    pg_policy_catalog_destroy(pols, n_pols);
    pg_security_config_destroy(&sec);
}

/* ── 7. Acumulación multi-error: 3 fallos distintos reportados de una. */
static void test_multi_error_accumulation(void)
{
    write_temp_ini(
        "[policy:no_actions]\n"
        "metric = cpu_percent\n"
        "threshold = 50.0\n"
        "\n"
        "[policy:bad_metric]\n"
        "metric = disk_read\n"
        "threshold = 50.0\n"
        "actions = warn\n"
        "\n"
        "[policy:inverted]\n"
        "metric = cpu_percent\n"
        "threshold = 30.0\n"
        "threshold_low = 70.0\n"
        "actions = warn\n");

    pg_policy_t *pols = NULL;
    size_t n_pols = 0;
    pg_global_config_t g = { 0 };
    pg_security_config_t sec = { 0 };
    size_t n_errs = 0;

    int rc = pg_policy_catalog_load(g_tmp_path, &pols, &n_pols, &g, &sec, &n_errs);
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, rc);
    TEST_ASSERT_EQUAL_UINT(3, n_errs);
}

int main(void)
{
    /* Silenciar stderr para que los mensajes esperados del parser no
     * ensucien la salida del runner. Los asserts verifican vía n_errs. */
    freopen("/dev/null", "w", stderr);

    UNITY_BEGIN();
    RUN_TEST(test_happy_ini_parses_all_fields);
    RUN_TEST(test_missing_actions_rejected);
    RUN_TEST(test_unknown_metric_rejected);
    RUN_TEST(test_threshold_inversion_rejected);
    RUN_TEST(test_security_section_accepted);
    RUN_TEST(test_global_defaults);
    RUN_TEST(test_multi_error_accumulation);
    return UNITY_END();
}
