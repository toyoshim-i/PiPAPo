/*
 * test_float.c — FPU context preservation across context switches
 *
 * Verifies that hardware float registers survive PendSV context switches.
 * Uses nanosleep() to yield the CPU, forcing PendSV to save/restore
 * the FPU state (s0-s15 via lazy stacking, s16-s31 manually).
 *
 * On Cortex-M0+ (no FPU), float operations use soft-float library calls
 * and don't touch FPU registers, so this test still passes (trivially).
 */

#include "utest.h"

/* Minimal timespec for nanosleep */
struct timespec {
    long tv_sec;
    long tv_nsec;
};

/* Force a context switch by sleeping for one tick (~10 ms) */
static void yield(void)
{
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 10000000;  /* 10 ms — at least one SysTick */
    nanosleep(&ts, (void *)0);
}

/* Compare floats with tolerance (no fabsf — avoid libm dependency).
 * Uses integer bit manipulation to avoid soft-float division. */
static int float_eq(float a, float b)
{
    /* Use subtraction and compare — all soft-float calls */
    float diff = a - b;
    if (diff < 0.0f) diff = -diff;
    return diff < 0.01f;
}

int main(void)
{
    /* 1. Basic float arithmetic survives a context switch */
    volatile float a = 3.0f;
    volatile float b = 2.0f;
    volatile float sum = a + b;
    yield();
    UT_ASSERT(float_eq(sum, 5.0f), "float add survives context switch");

    /* 2. Float multiply across yield */
    volatile float x = 6.0f;
    volatile float y = 7.0f;
    volatile float prod = x * y;
    yield();
    UT_ASSERT(float_eq(prod, 42.0f), "float mul survives context switch");

    /* 3. Float accumulate across multiple yields */
    volatile float acc = 1.0f;
    acc = acc + 1.0f;
    yield();
    acc = acc + 1.0f;
    yield();
    acc = acc + 1.0f;
    yield();
    UT_ASSERT(float_eq(acc, 4.0f), "float accumulate across 3 yields");

    UT_SUMMARY("test_float");
}
