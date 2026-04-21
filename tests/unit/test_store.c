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

static void test_buffer_wraparound_and_buf_cap(void)
{
    pg_store_t *s = NULL;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_init(&s, 3));

    pg_proc_id_t id = { .pid = 100, .starttime = 500 };
    for (unsigned long long u = 1; u <= 5; u++) {
        pg_raw_sample_t in = make_sample(100, 500, "x", u);
        TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_insert(s, &in));
    }

    /* buf_cap=10 > count=3 → devuelve los 3 más recientes: utimes [3,4,5]. */
    pg_raw_sample_t buf[10];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL_INT(PG_OK,
        pg_store_get_history(s, id, buf, 10, &out_len));
    TEST_ASSERT_EQUAL_UINT(3, out_len);
    TEST_ASSERT_EQUAL_UINT64(3, buf[0].utime);
    TEST_ASSERT_EQUAL_UINT64(4, buf[1].utime);
    TEST_ASSERT_EQUAL_UINT64(5, buf[2].utime);

    /* buf_cap=2 < count=3 → devuelve los 2 más recientes: utimes [4,5]. */
    out_len = 0;
    TEST_ASSERT_EQUAL_INT(PG_OK,
        pg_store_get_history(s, id, buf, 2, &out_len));
    TEST_ASSERT_EQUAL_UINT(2, out_len);
    TEST_ASSERT_EQUAL_UINT64(4, buf[0].utime);
    TEST_ASSERT_EQUAL_UINT64(5, buf[1].utime);

    pg_store_destroy(s);
}

static void test_multiple_entries_independent(void)
{
    pg_store_t *s = NULL;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_init(&s, 16));

    pg_proc_id_t a = { .pid = 1, .starttime = 100 };
    pg_proc_id_t b = { .pid = 2, .starttime = 200 };
    pg_proc_id_t c = { .pid = 1, .starttime = 300 }; /* mismo pid que a */

    pg_raw_sample_t sa1 = make_sample(a.pid, a.starttime, "a", 10);
    pg_raw_sample_t sa2 = make_sample(a.pid, a.starttime, "a", 11);
    pg_raw_sample_t sb1 = make_sample(b.pid, b.starttime, "b", 20);
    pg_raw_sample_t sb2 = make_sample(b.pid, b.starttime, "b", 21);
    pg_raw_sample_t sc1 = make_sample(c.pid, c.starttime, "c", 30);
    pg_raw_sample_t sc2 = make_sample(c.pid, c.starttime, "c", 31);

    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_insert(s, &sa1));
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_insert(s, &sb1));
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_insert(s, &sc1));
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_insert(s, &sa2));
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_insert(s, &sb2));
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_insert(s, &sc2));

    pg_raw_sample_t buf[4];
    size_t out_len = 0;

    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_get_history(s, a, buf, 4, &out_len));
    TEST_ASSERT_EQUAL_UINT(2, out_len);
    TEST_ASSERT_EQUAL_STRING("a", buf[0].comm);
    TEST_ASSERT_EQUAL_UINT64(10, buf[0].utime);
    TEST_ASSERT_EQUAL_UINT64(11, buf[1].utime);

    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_get_history(s, b, buf, 4, &out_len));
    TEST_ASSERT_EQUAL_UINT(2, out_len);
    TEST_ASSERT_EQUAL_STRING("b", buf[0].comm);
    TEST_ASSERT_EQUAL_UINT64(20, buf[0].utime);
    TEST_ASSERT_EQUAL_UINT64(21, buf[1].utime);

    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_get_history(s, c, buf, 4, &out_len));
    TEST_ASSERT_EQUAL_UINT(2, out_len);
    TEST_ASSERT_EQUAL_STRING("c", buf[0].comm);
    TEST_ASSERT_EQUAL_UINT64(30, buf[0].utime);
    TEST_ASSERT_EQUAL_UINT64(31, buf[1].utime);

    pg_store_destroy(s);
}

static void test_get_history_unknown_id_returns_zero(void)
{
    pg_store_t *s = NULL;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_init(&s, 16));

    /* Inserta algo para que el store no esté vacío — el id buscado
     * sigue siendo desconocido. */
    pg_raw_sample_t known = make_sample(100, 500, "k", 1);
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_insert(s, &known));

    pg_proc_id_t unknown = { .pid = 999, .starttime = 999 };
    pg_raw_sample_t buf[4];
    size_t out_len = 42; /* sentinel para verificar que se sobrescribe */
    TEST_ASSERT_EQUAL_INT(PG_OK,
        pg_store_get_history(s, unknown, buf, 4, &out_len));
    TEST_ASSERT_EQUAL_UINT(0, out_len);

    pg_store_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_destroy_clean);
    RUN_TEST(test_insert_single_sample);
    RUN_TEST(test_buffer_wraparound_and_buf_cap);
    RUN_TEST(test_multiple_entries_independent);
    RUN_TEST(test_get_history_unknown_id_returns_zero);
    return UNITY_END();
}
