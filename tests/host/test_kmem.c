/*
 * test_kmem.c — Unit tests for src/kernel/mm/kmem.c
 *
 * kmem.c has no hardware dependencies — it only uses standard C
 * (stddef.h, __builtin_memcpy) and operates on caller-supplied memory.
 * All tests run cleanly on the host with no stubs required.
 */

#include "test_framework.h"
#include "kernel/mm/kmem.h"
#include <stdint.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────────────── */

#define OBJ_SIZE   16u
#define CAPACITY    8u

typedef struct { uint8_t data[OBJ_SIZE]; } obj_t;

/* Create a fresh pool backed by a local buffer */
static void make_pool(kmem_pool_t *pool, obj_t *storage, uint32_t cap)
{
    kmem_pool_init(pool, storage, sizeof(obj_t), cap);
}

/* ── Test cases ──────────────────────────────────────────────────────────── */

static void test_init_free_count(void)
{
    obj_t storage[CAPACITY];
    kmem_pool_t pool;
    make_pool(&pool, storage, CAPACITY);
    ASSERT_EQ(kmem_free_count(&pool), CAPACITY);
}

static void test_init_total(void)
{
    obj_t storage[CAPACITY];
    kmem_pool_t pool;
    make_pool(&pool, storage, CAPACITY);
    ASSERT_EQ(pool.total, CAPACITY);
}

static void test_alloc_returns_non_null(void)
{
    obj_t storage[CAPACITY];
    kmem_pool_t pool;
    make_pool(&pool, storage, CAPACITY);
    void *p = kmem_alloc(&pool);
    ASSERT_NOT_NULL(p);
}

static void test_alloc_decrements_free_count(void)
{
    obj_t storage[CAPACITY];
    kmem_pool_t pool;
    make_pool(&pool, storage, CAPACITY);
    kmem_alloc(&pool);
    ASSERT_EQ(kmem_free_count(&pool), CAPACITY - 1u);
}

static void test_alloc_returns_distinct_pointers(void)
{
    obj_t storage[CAPACITY];
    kmem_pool_t pool;
    make_pool(&pool, storage, CAPACITY);
    void *a = kmem_alloc(&pool);
    void *b = kmem_alloc(&pool);
    ASSERT(a != b, "two allocs should return different objects");
}

static void test_alloc_returns_pool_memory(void)
{
    obj_t storage[CAPACITY];
    kmem_pool_t pool;
    make_pool(&pool, storage, CAPACITY);
    void *p = kmem_alloc(&pool);
    /* Returned pointer must lie within the backing storage */
    ASSERT(p >= (void *)storage,                          "below storage base");
    ASSERT(p <  (void *)(storage + CAPACITY),             "above storage end");
}

static void test_alloc_exhaustion_returns_null(void)
{
    obj_t storage[CAPACITY];
    kmem_pool_t pool;
    make_pool(&pool, storage, CAPACITY);

    for (uint32_t i = 0u; i < CAPACITY; i++)
        kmem_alloc(&pool);

    void *extra = kmem_alloc(&pool);
    ASSERT_NULL(extra);
    ASSERT_EQ(kmem_free_count(&pool), 0u);
}

static void test_free_increments_count(void)
{
    obj_t storage[CAPACITY];
    kmem_pool_t pool;
    make_pool(&pool, storage, CAPACITY);
    void *p = kmem_alloc(&pool);
    uint32_t before = kmem_free_count(&pool);
    kmem_free(&pool, p);
    ASSERT_EQ(kmem_free_count(&pool), before + 1u);
}

static void test_free_allows_realloc(void)
{
    obj_t storage[CAPACITY];
    kmem_pool_t pool;
    make_pool(&pool, storage, CAPACITY);
    void *p = kmem_alloc(&pool);
    kmem_free(&pool, p);
    void *q = kmem_alloc(&pool);
    ASSERT_NOT_NULL(q);
}

static void test_free_null_is_safe(void)
{
    obj_t storage[CAPACITY];
    kmem_pool_t pool;
    make_pool(&pool, storage, CAPACITY);
    /* Must not crash or corrupt state */
    kmem_free(&pool, NULL);
    ASSERT_EQ(kmem_free_count(&pool), CAPACITY);
}

static void test_alloc_all_free_all(void)
{
    obj_t storage[CAPACITY];
    kmem_pool_t pool;
    make_pool(&pool, storage, CAPACITY);

    void *ptrs[CAPACITY];
    for (uint32_t i = 0u; i < CAPACITY; i++)
        ptrs[i] = kmem_alloc(&pool);

    ASSERT_EQ(kmem_free_count(&pool), 0u);

    for (uint32_t i = 0u; i < CAPACITY; i++)
        kmem_free(&pool, ptrs[i]);

    ASSERT_EQ(kmem_free_count(&pool), CAPACITY);
    ASSERT_NOT_NULL(kmem_alloc(&pool)); /* pool is usable again */
}

static void test_zero_capacity_pool(void)
{
    kmem_pool_t pool;
    kmem_pool_init(&pool, NULL, OBJ_SIZE, 0u);
    ASSERT_EQ(kmem_free_count(&pool), 0u);
    ASSERT_NULL(kmem_alloc(&pool));
}

static void test_two_pools_independent(void)
{
    obj_t storageA[4];
    obj_t storageB[4];
    kmem_pool_t poolA, poolB;
    kmem_pool_init(&poolA, storageA, sizeof(obj_t), 4u);
    kmem_pool_init(&poolB, storageB, sizeof(obj_t), 4u);

    void *a = kmem_alloc(&poolA);
    ASSERT_EQ(kmem_free_count(&poolA), 3u);
    ASSERT_EQ(kmem_free_count(&poolB), 4u); /* B untouched */
    kmem_free(&poolA, a);
    ASSERT_EQ(kmem_free_count(&poolA), 4u);
    ASSERT_EQ(kmem_free_count(&poolB), 4u);
}

static void test_object_writable_after_alloc(void)
{
    obj_t storage[CAPACITY];
    kmem_pool_t pool;
    make_pool(&pool, storage, CAPACITY);
    obj_t *p = (obj_t *)kmem_alloc(&pool);
    /* Should be able to write to the returned object without crashing */
    memset(p->data, 0xAB, sizeof(p->data));
    ASSERT(p->data[0] == 0xAB, "write to allocated object should persist");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== test_kmem ===\n");

    TEST_GROUP("Initialisation");
    RUN_TEST(test_init_free_count);
    RUN_TEST(test_init_total);
    RUN_TEST(test_zero_capacity_pool);

    TEST_GROUP("Allocation");
    RUN_TEST(test_alloc_returns_non_null);
    RUN_TEST(test_alloc_decrements_free_count);
    RUN_TEST(test_alloc_returns_distinct_pointers);
    RUN_TEST(test_alloc_returns_pool_memory);
    RUN_TEST(test_alloc_exhaustion_returns_null);
    RUN_TEST(test_object_writable_after_alloc);

    TEST_GROUP("Free");
    RUN_TEST(test_free_increments_count);
    RUN_TEST(test_free_allows_realloc);
    RUN_TEST(test_free_null_is_safe);
    RUN_TEST(test_alloc_all_free_all);

    TEST_GROUP("Pool isolation");
    RUN_TEST(test_two_pools_independent);

    TEST_SUMMARY();
}
