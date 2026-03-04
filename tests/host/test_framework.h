/*
 * test_framework.h — Minimal unit-test framework for PPAP host tests
 *
 * Usage:
 *   Include this header in exactly one .c file per test executable.
 *   Define test functions (void fn(void)), register them with RUN_TEST(),
 *   call TEST_SUMMARY() at the end of main() to print results and return
 *   an exit code (0 = all passed, 1 = at least one failure).
 *
 * Example:
 *   static void test_alloc(void) {
 *       void *p = page_alloc();
 *       ASSERT(p != NULL, "alloc should return non-NULL");
 *   }
 *   int main(void) {
 *       RUN_TEST(test_alloc);
 *       TEST_SUMMARY();
 *   }
 */

#ifndef PPAP_TEST_FRAMEWORK_H
#define PPAP_TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>

/* Counters — each test executable has its own copies */
static int tf_fail = 0;
static int tf_asserts = 0;
static const char *tf_current = "(none)";

/*
 * ASSERT(cond, msg) — check a condition within a test function.
 * On failure: prints the test name, source location, and message; increments
 * the fail counter.  Execution of the current test function continues.
 */
#define ASSERT(cond, msg) do {                                              \
    tf_asserts++;                                                           \
    if (!(cond)) {                                                          \
        fprintf(stderr, "  FAIL  %-40s  %s:%d  %s\n",                      \
                tf_current, __FILE__, __LINE__, (msg));                     \
        tf_fail++;                                                          \
    }                                                                       \
} while (0)

/*
 * ASSERT_EQ(a, b) — check two integer values are equal.
 * Prints both values on failure.
 */
#define ASSERT_EQ(a, b) do {                                                \
    tf_asserts++;                                                           \
    long _a = (long)(a), _b = (long)(b);                                   \
    if (_a != _b) {                                                         \
        fprintf(stderr, "  FAIL  %-40s  %s:%d  expected %ld, got %ld\n",   \
                tf_current, __FILE__, __LINE__, _b, _a);                   \
        tf_fail++;                                                          \
    }                                                                       \
} while (0)

/*
 * ASSERT_NULL(p) / ASSERT_NOT_NULL(p) — pointer checks.
 */
#define ASSERT_NULL(p) ASSERT((p) == NULL, #p " should be NULL")
#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL, #p " should not be NULL")

/*
 * RUN_TEST(fn) — register and run a test function.
 * Prints the test name before running.
 */
#define RUN_TEST(fn) do {                                                   \
    tf_current = #fn;                                                       \
    printf("  RUN   %s\n", #fn);                                           \
    fn();                                                                   \
} while (0)

/*
 * TEST_GROUP(name) — print a section header for a group of related tests.
 */
#define TEST_GROUP(name) printf("\n%s\n", (name))

/*
 * TEST_SUMMARY() — print totals and return the exit code.
 * Call as the last statement in main().
 */
#define TEST_SUMMARY() do {                                                 \
    printf("\n%d assertions, %d passed, %d failed\n",                      \
           tf_asserts, tf_asserts - tf_fail, tf_fail);                     \
    return (tf_fail > 0) ? 1 : 0;                                          \
} while (0)

#endif /* PPAP_TEST_FRAMEWORK_H */
