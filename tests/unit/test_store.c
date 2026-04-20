#include "unity.h"
#include "store.h"
#include "pg_types.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_init_destroy_clean(void)
{
    pg_store_t *s = NULL;
    TEST_ASSERT_EQUAL_INT(PG_OK, pg_store_init(&s, 16));
    TEST_ASSERT_NOT_NULL(s);
    pg_store_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_destroy_clean);
    return UNITY_END();
}
