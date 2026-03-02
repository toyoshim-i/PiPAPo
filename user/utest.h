/*
 * utest.h — On-target user-space test framework for PPAP
 *
 * Minimal assertion macros that report results via write(1, ...).
 * No libc dependency — uses only SVC wrappers from syscall.h.
 *
 * Usage:
 *   #include "utest.h"
 *   int main(void) {
 *       UT_ASSERT(1, "always passes");
 *       UT_ASSERT_EQ(2+2, 4);
 *       UT_SUMMARY("test_name");
 *   }
 */

#ifndef PPAP_UTEST_H
#define PPAP_UTEST_H

#include "syscall.h"

static int ut_fail = 0;
static int ut_total = 0;

/* Write a string literal to stdout */
#define UT_PRINT(s) write(1, (s), sizeof(s) - 1)

/* Convert an int to decimal and print.
 * Uses subtraction instead of division — Cortex-M0+ has no hardware divide
 * and we don't link libgcc (__aeabi_idiv). */
static inline void ut_print_int(int v)
{
    if (v < 0) { UT_PRINT("-"); v = -v; }
    if (v == 0) { UT_PRINT("0"); return; }
    static const int powers[] = {1000000000,100000000,10000000,1000000,
                                 100000,10000,1000,100,10,1};
    int started = 0;
    for (int p = 0; p < 10; p++) {
        int d = 0;
        while (v >= powers[p]) { v -= powers[p]; d++; }
        if (d || started) {
            char c = '0' + d;
            write(1, &c, 1);
            started = 1;
        }
    }
}

/* Core assertion: check condition, print PASS/FAIL with message */
#define UT_ASSERT(cond, msg) do {                    \
    ut_total++;                                      \
    if (!(cond)) {                                   \
        ut_fail++;                                   \
        UT_PRINT("  FAIL  " msg " (" __FILE__ ":"); \
        ut_print_int(__LINE__);                      \
        UT_PRINT(")\n");                             \
    }                                                \
} while (0)

/* Check two values are equal — print both on failure */
#define UT_ASSERT_EQ(a, b) do {                      \
    ut_total++;                                      \
    long _a = (long)(a), _b = (long)(b);             \
    if (_a != _b) {                                  \
        ut_fail++;                                   \
        UT_PRINT("  FAIL  expected ");               \
        ut_print_int((int)_b);                       \
        UT_PRINT(" got ");                           \
        ut_print_int((int)_a);                       \
        UT_PRINT(" (" __FILE__ ":");                 \
        ut_print_int(__LINE__);                      \
        UT_PRINT(")\n");                             \
    }                                                \
} while (0)

/* Print summary and exit with 0 (all pass) or 1 (failures) */
#define UT_SUMMARY(name) do {                        \
    UT_PRINT(name ": ");                             \
    ut_print_int(ut_total);                          \
    UT_PRINT(" tests, ");                            \
    ut_print_int(ut_fail);                           \
    UT_PRINT(" failed\n");                           \
    return ut_fail ? 1 : 0;                          \
} while (0)

#endif /* PPAP_UTEST_H */
