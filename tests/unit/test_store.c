#include "unity.h"
#include "store.h"
#include "pg_types.h"

#include <string.h>

void setUp(void)
{
}

void tearDown(void)
{
}

/* Construye un sample mínimo con id arbitrario; utime actúa como marca. */
static pg_raw_sample_t make_sample(pid_t pid, unsigned long long starttime,
                                   const char *comm,
                                   unsigned long long utime)
{
    pg_raw_sample_t s = {0};
    s.id.pid = pid;
    s.id.starttime = starttime;
    strncpy(s.comm, comm, PG_COMM_MAX - 1);
    s.utime = utime;
    return s;
}

static void test_init_destroy_clean(void)
{
    pg_store_t *s = NULL;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_init(&s, 16));
    TEST_ASSERT_NOT_NULL(s);
    pg_store_destroy(s);
}

static void test_insert_single_sample(void)
{
    pg_store_t *s = NULL;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_init(&s, 16));

    pg_raw_sample_t in = make_sample(100, 500, "x", 7);
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_insert(s, &in));

    pg_raw_sample_t buf[8];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL_INT(PG_OK,
        pg_store_get_history(s, in.id, buf, 8, &out_len));
    TEST_ASSERT_EQUAL_UINT(1, out_len);
    TEST_ASSERT_EQUAL_INT(100, buf[0].id.pid);
    TEST_ASSERT_EQUAL_UINT64(500, buf[0].id.starttime);
    TEST_ASSERT_EQUAL_STRING("x", buf[0].comm);
    TEST_ASSERT_EQUAL_UINT64(7, buf[0].utime);

    pg_store_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_destroy_clean);
    RUN_TEST(test_insert_single_sample);
    return UNITY_END();
}
