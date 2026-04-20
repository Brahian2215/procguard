#include "unity.h"
#include "rank.h"

#include <stdlib.h>
#include <string.h>

/* --- Fixture helpers ----------------------------------------------------- */

static ranked_t make_ranked(pid_t pid, float cpu)
{
    ranked_t r;
    memset(&r, 0, sizeof(r));
    r.pid = pid;
    r.cpu = cpu;
    return r;
}

void setUp(void)    { }
void tearDown(void) { }

/* --- Tests --------------------------------------------------------------- */

static void test_orders_descending(void)
{
    /* qsort con cmp descendente: {1.0, 3.0, 2.0} → {3.0, 2.0, 1.0}. */
    ranked_t arr[3] = {
        make_ranked(1, 1.0f),
        make_ranked(2, 3.0f),
        make_ranked(3, 2.0f),
    };
    qsort(arr, 3, sizeof(arr[0]), pg_rank_cmp_cpu_desc);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, arr[0].cpu);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, arr[1].cpu);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, arr[2].cpu);
}

static void test_orders_fractional_differences(void)
{
    /* Guard contra implementación naive `(int)(fa-fb)` que trunca a 0
     * cuando |fa-fb| < 1: {0.3, 0.7, 0.1} debe quedar {0.7, 0.3, 0.1}. */
    ranked_t arr[3] = {
        make_ranked(1, 0.3f),
        make_ranked(2, 0.7f),
        make_ranked(3, 0.1f),
    };
    qsort(arr, 3, sizeof(arr[0]), pg_rank_cmp_cpu_desc);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.7f, arr[0].cpu);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.3f, arr[1].cpu);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.1f, arr[2].cpu);
}

static void test_equal_cpu_returns_zero(void)
{
    /* Invariante simétrico: cmp(a,a) == 0. Documentado ADR-016. */
    ranked_t a = make_ranked(1, 5.0f);
    ranked_t b = make_ranked(2, 5.0f);
    TEST_ASSERT_EQUAL_INT(0, pg_rank_cmp_cpu_desc(&a, &b));
    TEST_ASSERT_EQUAL_INT(0, pg_rank_cmp_cpu_desc(&b, &a));
}

/* --- Runner -------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_orders_descending);
    RUN_TEST(test_orders_fractional_differences);
    RUN_TEST(test_equal_cpu_returns_zero);
    return UNITY_END();
}
