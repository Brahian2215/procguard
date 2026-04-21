#include "unity.h"
#include "metrics.h"
#include "pg_types.h"

#include <string.h>

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

/* Happy path: idle (0 jiffies/s ⇒ 0%) y carga ligera (1 jiffy en 1s con
 * hz=100 ⇒ 1%). Cubre la fórmula básica de CPU%. */
static void test_happy_path(void)
{
    pg_raw_sample_t idle_prev = make_sample(1, 100, 0, 0, 0);
    pg_raw_sample_t idle_curr = make_sample(1, 100, 0, 0, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f,
        pg_metrics_cpu_percent(&idle_prev, &idle_curr, 100, 4));

    pg_raw_sample_t one_prev = make_sample(1, 100, 0, 0, 0);
    pg_raw_sample_t one_curr = make_sample(1, 100, 1, 0, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f,
        pg_metrics_cpu_percent(&one_prev, &one_curr, 100, 4));
}

/* 500 jiffies en 1 s con hz=100 ⇒ 500% sin clamp. Clamp = 100*ncpus. */
static void test_clamp_at_ncpus(void)
{
    pg_raw_sample_t prev = make_sample(1, 100, 0, 0, 0);
    pg_raw_sample_t curr = make_sample(1, 100, 500, 0, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 400.0f,
        pg_metrics_cpu_percent(&prev, &curr, 100, 4));
}

/* timestamp_ms idéntico ⇒ evita div-by-zero; retorna 0.0f. */
static void test_zero_elapsed(void)
{
    pg_raw_sample_t prev = make_sample(1, 100, 10, 10, 5000);
    pg_raw_sample_t curr = make_sample(1, 100, 20, 20, 5000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f,
        pg_metrics_cpu_percent(&prev, &curr, 100, 4));
}

/* Sentinel -1.0f cuando las dos muestras no pertenecen al mismo proceso
 * (pid o starttime distintos) o alguna es NULL. */
static void test_sentinel_on_id_mismatch_or_null(void)
{
    pg_raw_sample_t a = make_sample(42, 1000, 0, 0, 0);
    pg_raw_sample_t b_pid   = make_sample(43, 1000, 50, 0, 1000);
    pg_raw_sample_t b_start = make_sample(42, 2000, 50, 0, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f,
        pg_metrics_cpu_percent(&a, &b_pid, 100, 4));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f,
        pg_metrics_cpu_percent(&a, &b_start, 100, 4));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f,
        pg_metrics_cpu_percent(NULL, &a, 100, 4));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f,
        pg_metrics_cpu_percent(&a, NULL, 100, 4));
}

/* Mismo id pero utime+stime decrecen ⇒ violación de monotonía del kernel;
 * M3 retorna sentinel en vez de wrap unsigned que dispararía false alarm. */
static void test_sentinel_on_underflow(void)
{
    pg_raw_sample_t prev = make_sample(1, 100, 200, 100, 0);
    pg_raw_sample_t curr = make_sample(1, 100, 100, 50,  1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f,
        pg_metrics_cpu_percent(&prev, &curr, 100, 4));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_happy_path);
    RUN_TEST(test_clamp_at_ncpus);
    RUN_TEST(test_zero_elapsed);
    RUN_TEST(test_sentinel_on_id_mismatch_or_null);
    RUN_TEST(test_sentinel_on_underflow);
    return UNITY_END();
}
