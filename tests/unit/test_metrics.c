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

static pg_raw_sample_t make_io_sample(pid_t pid,
                                      unsigned long long starttime,
                                      unsigned long long timestamp_ms,
                                      unsigned long long rchar,
                                      unsigned long long wchar,
                                      unsigned long long read_bytes,
                                      unsigned long long write_bytes)
{
    pg_raw_sample_t s = make_sample(pid, starttime, 0, 0, timestamp_ms);
    s.rchar = rchar;
    s.wchar = wchar;
    s.read_bytes = read_bytes;
    s.write_bytes = write_bytes;
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

/* Tasas I/O: 4 counters suben a ritmos distintos en 1s con hz irrelevante
 * (hz sólo es argumento de CPU%). Verifica la aritmética básica por campo. */
static void test_io_rates_happy_mixed(void)
{
    pg_raw_sample_t prev = make_io_sample(1, 100, 0,       0,     0,     0,    0);
    pg_raw_sample_t curr = make_io_sample(1, 100, 1000, 1000,   500,  2000,  100);
    pg_io_rates_t r = { -9.0f, -9.0f, -9.0f, -9.0f };
    pg_metrics_io_rates(&prev, &curr, &r);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1000.0f, r.rchar_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f,  500.0f, r.wchar_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2000.0f, r.read_bytes_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f,  100.0f, r.write_bytes_per_s);
}

/* pid o starttime distinto ⇒ los 4 campos a -1.0f. */
static void test_io_rates_id_mismatch_sentinel_all(void)
{
    pg_raw_sample_t a        = make_io_sample(42, 1000, 0,    100,  200,  300,  400);
    pg_raw_sample_t b_pid    = make_io_sample(43, 1000, 1000, 999,  999,  999,  999);
    pg_raw_sample_t b_start  = make_io_sample(42, 2000, 1000, 999,  999,  999,  999);

    pg_io_rates_t r = { 0 };
    pg_metrics_io_rates(&a, &b_pid, &r);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, r.rchar_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, r.wchar_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, r.read_bytes_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, r.write_bytes_per_s);

    memset(&r, 0, sizeof(r));
    pg_metrics_io_rates(&a, &b_start, &r);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, r.rchar_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, r.wchar_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, r.read_bytes_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, r.write_bytes_per_s);
}

/* prev o curr NULL ⇒ los 4 campos a -1.0f. out NULL ⇒ no-op (no-crash). */
static void test_io_rates_null_sentinel_all(void)
{
    pg_raw_sample_t a = make_io_sample(1, 100, 1000, 10, 20, 30, 40);
    pg_io_rates_t r   = { 0 };

    pg_metrics_io_rates(NULL, &a, &r);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, r.rchar_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, r.wchar_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, r.read_bytes_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, r.write_bytes_per_s);

    memset(&r, 0, sizeof(r));
    pg_metrics_io_rates(&a, NULL, &r);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, r.rchar_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, r.wchar_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, r.read_bytes_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, r.write_bytes_per_s);

    /* out==NULL: no-op, no crash. */
    pg_metrics_io_rates(&a, &a, NULL);
}

/* Underflow aislado en read_bytes: sólo ese campo en -1.0f. Los otros
 * tres son válidos — los counters de /proc/[pid]/io son independientes y
 * descartar los cuatro perdería información. */
static void test_io_rates_single_underflow(void)
{
    pg_raw_sample_t prev = make_io_sample(1, 100, 0,    10,  20,  500,  40);
    pg_raw_sample_t curr = make_io_sample(1, 100, 1000, 60,  70,  300,  90);
    pg_io_rates_t r = { 0 };
    pg_metrics_io_rates(&prev, &curr, &r);
    TEST_ASSERT_FLOAT_WITHIN(0.001f,  50.0f, r.rchar_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f,  50.0f, r.wchar_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f,  -1.0f, r.read_bytes_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f,  50.0f, r.write_bytes_per_s);
}

/* timestamp_ms idéntico ⇒ los 4 campos a 0.0f (espejo CPU%). */
static void test_io_rates_zero_elapsed(void)
{
    pg_raw_sample_t prev = make_io_sample(1, 100, 5000, 10, 20, 30, 40);
    pg_raw_sample_t curr = make_io_sample(1, 100, 5000, 99, 99, 99, 99);
    pg_io_rates_t r = { -9.0f, -9.0f, -9.0f, -9.0f };
    pg_metrics_io_rates(&prev, &curr, &r);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, r.rchar_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, r.wchar_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, r.read_bytes_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, r.write_bytes_per_s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_happy_path);
    RUN_TEST(test_clamp_at_ncpus);
    RUN_TEST(test_zero_elapsed);
    RUN_TEST(test_sentinel_on_id_mismatch_or_null);
    RUN_TEST(test_sentinel_on_underflow);
    RUN_TEST(test_io_rates_happy_mixed);
    RUN_TEST(test_io_rates_id_mismatch_sentinel_all);
    RUN_TEST(test_io_rates_null_sentinel_all);
    RUN_TEST(test_io_rates_single_underflow);
    RUN_TEST(test_io_rates_zero_elapsed);
    return UNITY_END();
}
