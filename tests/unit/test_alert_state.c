#include "unity.h"
#include "alert_state.h"
#include "store.h"
#include "pg_types.h"

#include <stdlib.h>
#include <string.h>

void setUp(void) { }
void tearDown(void) { }

static pg_proc_id_t make_id(pid_t pid, unsigned long long starttime)
{
    pg_proc_id_t id = { .pid = pid, .starttime = starttime };
    return id;
}

static pg_raw_sample_t make_sample(pid_t pid, unsigned long long starttime)
{
    pg_raw_sample_t s = {0};
    s.id.pid = pid;
    s.id.starttime = starttime;
    return s;
}

/* ── 1. init + destroy sin upsert: no leaks bajo ASAN. ──────────────── */
static void test_init_destroy_clean(void)
{
    pg_alert_state_registry_t *r = NULL;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_alert_state_registry_init(&r));
    TEST_ASSERT_NOT_NULL(r);
    pg_alert_state_registry_destroy(r);
}

/* ── 2. Upsert crea zero-init; segundo upsert devuelve mismo puntero
 *      y conserva mutaciones intermedias del caller. ────────────────── */
static void test_upsert_creates_and_preserves(void)
{
    pg_alert_state_registry_t *r = NULL;
    pg_alert_state_t *s1 = NULL;
    pg_alert_state_t *s2 = NULL;

    TEST_ASSERT_EQUAL_INT(PG_OK, pg_alert_state_registry_init(&r));

    TEST_ASSERT_EQUAL_INT(PG_OK,
        pg_alert_state_upsert(r, make_id(100, 500), 0, &s1));
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_EQUAL_UINT(0, s1->persistence);
    TEST_ASSERT_EQUAL_UINT(0, s1->hysteresis);
    TEST_ASSERT_EQUAL_UINT64(0, s1->cooldown_until_ms);
    TEST_ASSERT_EQUAL_INT(0, s1->escalation_level);
    TEST_ASSERT_FALSE(s1->deactivated_since_last_act);

    s1->persistence = 5;
    s1->cooldown_until_ms = 1234;
    s1->escalation_level = 2;
    s1->deactivated_since_last_act = true;

    TEST_ASSERT_EQUAL_INT(PG_OK,
        pg_alert_state_upsert(r, make_id(100, 500), 0, &s2));
    TEST_ASSERT_EQUAL_PTR(s1, s2);
    TEST_ASSERT_EQUAL_UINT(5, s2->persistence);
    TEST_ASSERT_EQUAL_UINT64(1234, s2->cooldown_until_ms);
    TEST_ASSERT_EQUAL_INT(2, s2->escalation_level);
    TEST_ASSERT_TRUE(s2->deactivated_since_last_act);

    pg_alert_state_registry_destroy(r);
}

/* ── 3. Mismo pid, distinto starttime → entry independiente (ADR-005). */
static void test_starttime_change_creates_new_entry(void)
{
    pg_alert_state_registry_t *r = NULL;
    pg_alert_state_t *s_old = NULL;
    pg_alert_state_t *s_new = NULL;

    TEST_ASSERT_EQUAL_INT(PG_OK, pg_alert_state_registry_init(&r));

    TEST_ASSERT_EQUAL_INT(PG_OK,
        pg_alert_state_upsert(r, make_id(100, 500), 0, &s_old));
    s_old->persistence = 7;

    TEST_ASSERT_EQUAL_INT(PG_OK,
        pg_alert_state_upsert(r, make_id(100, 999), 0, &s_new));

    TEST_ASSERT_TRUE_MESSAGE(s_old != s_new,
        "starttime distinto debe crear entry independiente");
    TEST_ASSERT_EQUAL_UINT(0, s_new->persistence);
    TEST_ASSERT_EQUAL_UINT(7, s_old->persistence);

    pg_alert_state_registry_destroy(r);
}

/* ── 4. Mismo id, distinto policy_index → dos entries. ──────────────── */
static void test_different_policy_index_creates_separate_state(void)
{
    pg_alert_state_registry_t *r = NULL;
    pg_alert_state_t *s_p0 = NULL;
    pg_alert_state_t *s_p1 = NULL;

    TEST_ASSERT_EQUAL_INT(PG_OK, pg_alert_state_registry_init(&r));

    TEST_ASSERT_EQUAL_INT(PG_OK,
        pg_alert_state_upsert(r, make_id(100, 500), 0, &s_p0));
    TEST_ASSERT_EQUAL_INT(PG_OK,
        pg_alert_state_upsert(r, make_id(100, 500), 1, &s_p1));

    TEST_ASSERT_TRUE_MESSAGE(s_p0 != s_p1,
        "policy_index distinto debe crear entry independiente");
    s_p0->hysteresis = 3;
    TEST_ASSERT_EQUAL_UINT(0, s_p1->hysteresis);

    pg_alert_state_registry_destroy(r);
}

/* ── 5. Lookup miss → NULL; lookup hit → mismo puntero que upsert. ──── */
static void test_lookup_returns_null_for_missing(void)
{
    pg_alert_state_registry_t *r = NULL;
    pg_alert_state_t *s_up = NULL;

    TEST_ASSERT_EQUAL_INT(PG_OK, pg_alert_state_registry_init(&r));

    TEST_ASSERT_NULL(pg_alert_state_lookup(r, make_id(100, 500), 0));

    TEST_ASSERT_EQUAL_INT(PG_OK,
        pg_alert_state_upsert(r, make_id(100, 500), 0, &s_up));

    TEST_ASSERT_EQUAL_PTR(s_up,
        pg_alert_state_lookup(r, make_id(100, 500), 0));
    TEST_ASSERT_NULL(pg_alert_state_lookup(r, make_id(100, 500), 1));
    TEST_ASSERT_NULL(pg_alert_state_lookup(r, make_id(101, 500), 0));

    pg_alert_state_registry_destroy(r);
}

/* ── 6. gc libera entries cuyo id no está en el store (ADR-013). ────── */
static void test_gc_releases_absent_from_store(void)
{
    pg_alert_state_registry_t *r = NULL;
    pg_store_t *st = NULL;
    pg_alert_state_t *s = NULL;

    TEST_ASSERT_EQUAL_INT(PG_OK, pg_alert_state_registry_init(&r));
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_init(&st, 8));

    TEST_ASSERT_EQUAL_INT(PG_OK,
        pg_alert_state_upsert(r, make_id(100, 500), 0, &s));
    TEST_ASSERT_NOT_NULL(pg_alert_state_lookup(r, make_id(100, 500), 0));

    pg_alert_state_gc(r, st);

    TEST_ASSERT_NULL(pg_alert_state_lookup(r, make_id(100, 500), 0));

    pg_store_destroy(st);
    pg_alert_state_registry_destroy(r);
}

/* ── 7. gc preserva entries cuyo id sigue en el store. ──────────────── */
static void test_gc_keeps_present_in_store(void)
{
    pg_alert_state_registry_t *r = NULL;
    pg_store_t *st = NULL;
    pg_alert_state_t *s = NULL;

    TEST_ASSERT_EQUAL_INT(PG_OK, pg_alert_state_registry_init(&r));
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_init(&st, 8));

    pg_raw_sample_t sample = make_sample(100, 500);
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_insert(st, &sample));

    TEST_ASSERT_EQUAL_INT(PG_OK,
        pg_alert_state_upsert(r, make_id(100, 500), 0, &s));
    s->persistence = 9;
    s->escalation_level = 1;

    pg_alert_state_gc(r, st);

    pg_alert_state_t *after = pg_alert_state_lookup(r, make_id(100, 500), 0);
    TEST_ASSERT_EQUAL_PTR(s, after);
    TEST_ASSERT_EQUAL_UINT(9, after->persistence);
    TEST_ASSERT_EQUAL_INT(1, after->escalation_level);

    pg_store_destroy(st);
    pg_alert_state_registry_destroy(r);
}

/* ── 8. Defensa NULL: PARSE en handles, no-op en destroy/gc. ────────── */
static void test_null_args_return_parse_err(void)
{
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE,
        pg_alert_state_registry_init(NULL));

    pg_alert_state_t *out = NULL;
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE,
        pg_alert_state_upsert(NULL, make_id(1, 1), 0, &out));

    pg_alert_state_registry_t *r = NULL;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_alert_state_registry_init(&r));
    TEST_ASSERT_EQUAL_INT(PG_ERR_PARSE,
        pg_alert_state_upsert(r, make_id(1, 1), 0, NULL));

    TEST_ASSERT_NULL(pg_alert_state_lookup(NULL, make_id(1, 1), 0));

    /* gc(NULL,_) y gc(_,NULL): no-crash, no efecto observable. */
    pg_alert_state_gc(NULL, NULL);
    pg_alert_state_gc(r, NULL);

    pg_alert_state_registry_destroy(r);
    pg_alert_state_registry_destroy(NULL);  /* no-op */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_destroy_clean);
    RUN_TEST(test_upsert_creates_and_preserves);
    RUN_TEST(test_starttime_change_creates_new_entry);
    RUN_TEST(test_different_policy_index_creates_separate_state);
    RUN_TEST(test_lookup_returns_null_for_missing);
    RUN_TEST(test_gc_releases_absent_from_store);
    RUN_TEST(test_gc_keeps_present_in_store);
    RUN_TEST(test_null_args_return_parse_err);
    return UNITY_END();
}
