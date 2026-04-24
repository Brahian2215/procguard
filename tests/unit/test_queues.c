#include "unity.h"
#include "queue.h"
#include "pg_types.h"

#include <string.h>

void setUp(void)    { }
void tearDown(void) { }

static pg_inotify_event_t make_ev(uint64_t ts, int wd, uint32_t mask)
{
    pg_inotify_event_t e = { .timestamp_ms = ts, .wd = wd, .mask = mask };
    return e;
}

/* pg_results_t: init + lock + unlock + destroy sin leaks ni deadlocks. El
 * mutex debe ser recursivo-safe para un solo hilo (lock tras unlock). */
static void test_results_init_lock_unlock_destroy(void)
{
    pg_results_t r;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_results_init(&r));
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_results_lock(&r));
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_results_unlock(&r));
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_results_lock(&r));
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_results_unlock(&r));
    pg_results_destroy(&r);
}

/* Inotify queue FIFO: push 3 eventos distintos, pop los 3 en orden. */
static void test_inotify_queue_push_pop_fifo(void)
{
    pg_inotify_event_queue_t q;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_init(&q, 8));

    pg_inotify_event_t a = make_ev(100, 1, 0x1);
    pg_inotify_event_t b = make_ev(200, 2, 0x2);
    pg_inotify_event_t c = make_ev(300, 3, 0x4);
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_push(&q, &a));
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_push(&q, &b));
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_push(&q, &c));
    TEST_ASSERT_EQUAL_UINT(3, q.count);

    pg_inotify_event_t drain[8];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_pop(&q, drain, 8, &n));
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_UINT64(100, drain[0].timestamp_ms);
    TEST_ASSERT_EQUAL_UINT64(200, drain[1].timestamp_ms);
    TEST_ASSERT_EQUAL_UINT64(300, drain[2].timestamp_ms);
    TEST_ASSERT_EQUAL_UINT(0, q.count);

    /* pop sobre cola vacía: PG_OK con n=0. */
    n = 42;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_pop(&q, drain, 8, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);

    pg_inotify_queue_destroy(&q);
}

/* Overflow: push capacity+1 eventos; el más antiguo debe caer y
 * q->dropped debe incrementarse. El orden de los restantes preserva
 * la cronología (FIFO con drop-oldest). */
static void test_inotify_queue_drops_oldest_on_overflow(void)
{
    pg_inotify_event_queue_t q;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_init(&q, 3));

    pg_inotify_event_t ev1 = make_ev(1, 1, 0);
    pg_inotify_event_t ev2 = make_ev(2, 2, 0);
    pg_inotify_event_t ev3 = make_ev(3, 3, 0);
    pg_inotify_event_t ev4 = make_ev(4, 4, 0);

    TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_push(&q, &ev1));
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_push(&q, &ev2));
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_push(&q, &ev3));
    TEST_ASSERT_EQUAL_UINT(0, q.dropped);
    /* overflow */
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_push(&q, &ev4));
    TEST_ASSERT_EQUAL_UINT(1, q.dropped);
    TEST_ASSERT_EQUAL_UINT(3, q.count);

    pg_inotify_event_t drain[3];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_pop(&q, drain, 3, &n));
    TEST_ASSERT_EQUAL_UINT(3, n);
    /* ev1 fue descartado; deben quedar ev2, ev3, ev4 en orden. */
    TEST_ASSERT_EQUAL_UINT64(2, drain[0].timestamp_ms);
    TEST_ASSERT_EQUAL_UINT64(3, drain[1].timestamp_ms);
    TEST_ASSERT_EQUAL_UINT64(4, drain[2].timestamp_ms);

    pg_inotify_queue_destroy(&q);
}

/* pop con buf_cap < count: drena sólo buf_cap y deja el resto para el
 * siguiente pop. Los elementos retornados son los más antiguos (FIFO). */
static void test_inotify_queue_partial_drain(void)
{
    pg_inotify_event_queue_t q;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_init(&q, 8));

    for (int i = 1; i <= 5; i++) {
        pg_inotify_event_t e = make_ev((uint64_t)i, i, 0);
        TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_push(&q, &e));
    }

    pg_inotify_event_t buf[2];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_pop(&q, buf, 2, &n));
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT64(1, buf[0].timestamp_ms);
    TEST_ASSERT_EQUAL_UINT64(2, buf[1].timestamp_ms);
    TEST_ASSERT_EQUAL_UINT(3, q.count);

    /* Siguientes pops agotan los 3 restantes. */
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_pop(&q, buf, 2, &n));
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT64(3, buf[0].timestamp_ms);
    TEST_ASSERT_EQUAL_UINT64(4, buf[1].timestamp_ms);
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_pop(&q, buf, 2, &n));
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT64(5, buf[0].timestamp_ms);

    pg_inotify_queue_destroy(&q);
}

/* Command queue: FIFO básico + tipo de mensaje. */
static void test_command_queue_push_pop_fifo(void)
{
    pg_command_queue_t q;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_command_queue_init(&q, 4));

    pg_command_t c1 = { .kind = PG_CMD_PAUSE,    .arg = 0    };
    pg_command_t c2 = { .kind = PG_CMD_KILL_PID, .arg = 1234 };
    pg_command_t c3 = { .kind = PG_CMD_QUIT,     .arg = 0    };
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_command_queue_push(&q, &c1));
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_command_queue_push(&q, &c2));
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_command_queue_push(&q, &c3));

    pg_command_t drain[4];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_command_queue_pop(&q, drain, 4, &n));
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_INT(PG_CMD_PAUSE,    drain[0].kind);
    TEST_ASSERT_EQUAL_INT(PG_CMD_KILL_PID, drain[1].kind);
    TEST_ASSERT_EQUAL_INT(1234,            drain[1].arg);
    TEST_ASSERT_EQUAL_INT(PG_CMD_QUIT,     drain[2].kind);

    pg_command_queue_destroy(&q);
}

/* NULL safety: init/push/pop con NULL retornan PG_ERR_PARSE; destroy(NULL)
 * es no-op (patrón free). */
static void test_queues_null_safety(void)
{
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, pg_results_init(NULL));
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, pg_results_lock(NULL));
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, pg_results_unlock(NULL));
    pg_results_destroy(NULL); /* no-op */

    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, pg_inotify_queue_init(NULL, 8));

    pg_inotify_event_queue_t q;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_inotify_queue_init(&q, 4));
    pg_inotify_event_t ev = make_ev(1, 1, 0);
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, pg_inotify_queue_push(NULL, &ev));
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, pg_inotify_queue_push(&q, NULL));

    pg_inotify_event_t buf[4];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, pg_inotify_queue_pop(NULL, buf, 4, &n));
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, pg_inotify_queue_pop(&q, NULL, 4, &n));
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, pg_inotify_queue_pop(&q, buf, 0,  &n));
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, pg_inotify_queue_pop(&q, buf, 4,  NULL));

    pg_inotify_queue_destroy(&q);
    pg_inotify_queue_destroy(NULL); /* no-op */

    /* init con capacity 0: inválido. */
    pg_inotify_event_queue_t bad;
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, pg_inotify_queue_init(&bad, 0));

    pg_command_queue_t cq;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_command_queue_init(&cq, 4));
    pg_command_t cmd = { .kind = PG_CMD_QUIT, .arg = 0 };
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, pg_command_queue_push(NULL, &cmd));
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE, pg_command_queue_push(&cq,  NULL));
    pg_command_queue_destroy(&cq);
    pg_command_queue_destroy(NULL); /* no-op */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_results_init_lock_unlock_destroy);
    RUN_TEST(test_inotify_queue_push_pop_fifo);
    RUN_TEST(test_inotify_queue_drops_oldest_on_overflow);
    RUN_TEST(test_inotify_queue_partial_drain);
    RUN_TEST(test_command_queue_push_pop_fifo);
    RUN_TEST(test_queues_null_safety);
    return UNITY_END();
}
