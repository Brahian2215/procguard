#include "unity.h"
#include "collector.h"
#include "pg_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TEST_PROC_BASE "/tmp/pg_test_proc"

/* --- Fixture helpers ----------------------------------------------------- */

static void write_stat(int pid, const char *content)
{
    char dir[128];
    char path[160];
    snprintf(dir, sizeof(dir), TEST_PROC_BASE "/%d", pid);
    snprintf(path, sizeof(path), "%s/stat", dir);

    mkdir(dir, 0755);

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fputs(content, f);
    fclose(f);
}

static const pg_raw_sample_t *find_sample(const pg_raw_sample_t *arr,
                                           size_t n, pid_t pid)
{
    for (size_t i = 0; i < n; i++) {
        if (arr[i].id.pid == pid) {
            return &arr[i];
        }
    }
    return NULL;
}

void setUp(void)
{
    /* Limpieza defensiva por si un run anterior dejó residuos. */
    system("rm -rf " TEST_PROC_BASE);
    mkdir(TEST_PROC_BASE, 0755);

    write_stat(100,
        "100 (bash) S 1 100 100 0 -1 0 0 0 0 0 150 50 0 0 20 0 1 0 12345 0 0\n");
    write_stat(200,
        "200 (nginx) S 1 200 200 0 -1 0 0 0 0 0 200 100 0 0 20 0 1 0 67890 0 0\n");
    write_stat(300,
        "300 (python3) R 100 300 300 0 -1 0 0 0 0 0 500 200 0 0 20 0 1 0 11111 0 0\n");
}

void tearDown(void)
{
    system("rm -rf " TEST_PROC_BASE);
}

/* --- Tests --------------------------------------------------------------- */

static void test_scan_finds_all_three(void)
{
    pg_collector_t *col = NULL;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_collector_init(&col, TEST_PROC_BASE));

    pg_raw_sample_t *out = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_collector_scan(col, &out, &n));
    TEST_ASSERT_EQUAL_size_t(3, n);

    TEST_ASSERT_NOT_NULL(find_sample(out, n, 100));
    TEST_ASSERT_NOT_NULL(find_sample(out, n, 200));
    TEST_ASSERT_NOT_NULL(find_sample(out, n, 300));

    free(out);
    pg_collector_destroy(col);
}

static void test_disappeared_process(void)
{
    pg_collector_t *col = NULL;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_collector_init(&col, TEST_PROC_BASE));

    pg_raw_sample_t *out1 = NULL;
    size_t n1 = 0;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_collector_scan(col, &out1, &n1));
    TEST_ASSERT_EQUAL_size_t(3, n1);
    free(out1);

    /* Eliminar pid 200 entre scans */
    system("rm -rf " TEST_PROC_BASE "/200");

    pg_raw_sample_t *out2 = NULL;
    size_t n2 = 0;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_collector_scan(col, &out2, &n2));
    TEST_ASSERT_EQUAL_size_t(2, n2);
    TEST_ASSERT_NULL(find_sample(out2, n2, 200));

    free(out2);
    pg_collector_destroy(col);
}

static void test_recycled_pid(void)
{
    pg_collector_t *col = NULL;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_collector_init(&col, TEST_PROC_BASE));

    pg_raw_sample_t *out1 = NULL;
    size_t n1 = 0;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_collector_scan(col, &out1, &n1));
    const pg_raw_sample_t *s1 = find_sample(out1, n1, 100);
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_EQUAL_UINT64(12345, s1->id.starttime);

    /* Reescribir 100/stat con starttime distinto (pid reciclado) */
    write_stat(100,
        "100 (bash) S 1 100 100 0 -1 0 0 0 0 0 1 1 0 0 20 0 1 0 99999 0 0\n");

    pg_raw_sample_t *out2 = NULL;
    size_t n2 = 0;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_collector_scan(col, &out2, &n2));
    const pg_raw_sample_t *s2 = find_sample(out2, n2, 100);
    TEST_ASSERT_NOT_NULL(s2);
    TEST_ASSERT_EQUAL_UINT64(99999, s2->id.starttime);

    /* Mismo pid, distinto starttime → ids distintos */
    TEST_ASSERT_EQUAL_INT(s1->id.pid, s2->id.pid);
    TEST_ASSERT_NOT_EQUAL(s1->id.starttime, s2->id.starttime);

    free(out1);
    free(out2);
    pg_collector_destroy(col);
}

static void test_malformed_stat_is_skipped(void)
{
    pg_collector_t *col = NULL;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_collector_init(&col, TEST_PROC_BASE));

    /* pid 400 con stat vacío (malformado) */
    write_stat(400, "");

    pg_raw_sample_t *out = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_collector_scan(col, &out, &n));
    TEST_ASSERT_EQUAL_size_t(3, n); /* 100, 200, 300 — 400 omitido */
    TEST_ASSERT_NULL(find_sample(out, n, 400));

    free(out);
    pg_collector_destroy(col);
}

static void test_parses_comm_with_internal_parens(void)
{
    pg_collector_t *col = NULL;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_collector_init(&col, TEST_PROC_BASE));

    /* comm = "weird ) name" — contiene ')' interno y espacios.
     * El parser correcto debe usar el ÚLTIMO ')' como delimitador. */
    write_stat(500,
        "500 (weird ) name) S 1 500 500 0 -1 0 0 0 0 0 10 5 0 0 20 0 1 0 22222 0 0\n");

    pg_raw_sample_t *out = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_collector_scan(col, &out, &n));
    TEST_ASSERT_EQUAL_size_t(4, n);

    const pg_raw_sample_t *s = find_sample(out, n, 500);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("weird ) name", s->comm);
    TEST_ASSERT_EQUAL_UINT64(22222, s->id.starttime);
    TEST_ASSERT_EQUAL_UINT64(10, s->utime);
    TEST_ASSERT_EQUAL_UINT64(5, s->stime);

    free(out);
    pg_collector_destroy(col);
}

/* --- Runner -------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_scan_finds_all_three);
    RUN_TEST(test_disappeared_process);
    RUN_TEST(test_recycled_pid);
    RUN_TEST(test_malformed_stat_is_skipped);
    RUN_TEST(test_parses_comm_with_internal_parens);
    return UNITY_END();
}
