#include "unity.h"
#include "metrics.h"
#include "pg_types.h"

#include <string.h>

/* --- Fixture helpers ----------------------------------------------------- */

/* Construye una muestra sin residuos de stack; campos no relevantes = 0. */
static pg_raw_sample_t make_sample(pid_t pid,
                                   unsigned long long starttime,
                                   unsigned long long utime,
                                   unsigned long long stime,
                                   unsigned long long timestamp_ms)
{
    pg_raw_sample_t s;
    memset(&s, 0, sizeof(s));
    s.id.pid = pid;
    s.id.starttime = starttime;
    s.utime = utime;
    s.stime = stime;
    s.timestamp_ms = timestamp_ms;
    return s;
}

void setUp(void)    { }
void tearDown(void) { }

/* --- Tests --------------------------------------------------------------- */

static void test_idle_process(void)
{
    /* Sin actividad en 1 s — 0 %. */
    pg_raw_sample_t prev = make_sample(1, 100, 0, 0, 0);
    pg_raw_sample_t curr = make_sample(1, 100, 0, 0, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f,
        pg_metrics_cpu_percent(&prev, &curr, 100, 4));
}

static void test_one_percent(void)
{
    /* 1 jiffy en 1 s con hz=100 ⇒ 1 %. */
    pg_raw_sample_t prev = make_sample(1, 100, 0, 0, 0);
    pg_raw_sample_t curr = make_sample(1, 100, 1, 0, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f,
        pg_metrics_cpu_percent(&prev, &curr, 100, 4));
}

static void test_recycled_pid_starttime_differs(void)
{
    /* Mismo pid, starttime distinto ⇒ proceso reciclado. */
    pg_raw_sample_t prev = make_sample(42, 1000, 0, 0, 0);
    pg_raw_sample_t curr = make_sample(42, 2000, 50, 0, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f,
        pg_metrics_cpu_percent(&prev, &curr, 100, 4));
}

static void test_recycled_pid_pid_differs(void)
{
    /* pid distinto ⇒ muestras no corresponden al mismo proceso. */
    pg_raw_sample_t prev = make_sample(42, 1000, 0, 0, 0);
    pg_raw_sample_t curr = make_sample(43, 1000, 50, 0, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f,
        pg_metrics_cpu_percent(&prev, &curr, 100, 4));
}

static void test_full_cpu_saturates_clamp(void)
{
    /* 500 jiffies en 1 s con hz=100 ⇒ 500 % sin clamp.
     * Clamp máximo = 100 * ncpus = 400 %. */
    pg_raw_sample_t prev = make_sample(1, 100, 0, 0, 0);
    pg_raw_sample_t curr = make_sample(1, 100, 500, 0, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 400.0f,
        pg_metrics_cpu_percent(&prev, &curr, 100, 4));
}

static void test_zero_elapsed(void)
{
    /* timestamp_ms idéntico ⇒ 0.0 f (no div-by-zero). */
    pg_raw_sample_t prev = make_sample(1, 100, 10, 10, 5000);
    pg_raw_sample_t curr = make_sample(1, 100, 20, 20, 5000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f,
        pg_metrics_cpu_percent(&prev, &curr, 100, 4));
}

static void test_null_args(void)
{
    pg_raw_sample_t s = make_sample(1, 100, 0, 0, 0);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f,
        pg_metrics_cpu_percent(NULL, &s, 100, 4));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f,
        pg_metrics_cpu_percent(&s, NULL, 100, 4));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f,
        pg_metrics_cpu_percent(NULL, NULL, 100, 4));
}

static void test_underflow_returns_sentinel(void)
{
    /* Mismo id pero jiffies decrecen ⇒ violación de invariante;
     * M3 no confía y retorna el sentinel (ADR-012). */
    pg_raw_sample_t prev = make_sample(1, 100, 200, 100, 0);
    pg_raw_sample_t curr = make_sample(1, 100, 100, 50,  1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f,
        pg_metrics_cpu_percent(&prev, &curr, 100, 4));
}

/* --- Runner -------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_idle_process);
    RUN_TEST(test_one_percent);
    RUN_TEST(test_recycled_pid_starttime_differs);
    RUN_TEST(test_recycled_pid_pid_differs);
    RUN_TEST(test_full_cpu_saturates_clamp);
    RUN_TEST(test_zero_elapsed);
    RUN_TEST(test_null_args);
    RUN_TEST(test_underflow_returns_sentinel);
    return UNITY_END();
}
