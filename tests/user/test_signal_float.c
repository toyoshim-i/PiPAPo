/*
 * test_signal_float.c — Signal delivery with FPU context preservation
 *
 * On Cortex-M33 with hardware FPU, verifies that:
 *   1. Float registers survive signal delivery + return (basic case)
 *   2. A signal handler can use floats without corrupting the caller's FPU state
 *   3. Float operations work correctly after signal handler returns
 *
 * On Cortex-M0+ (no FPU), this test still passes trivially since float
 * operations use soft-float library calls and don't touch FPU registers.
 */

#include <signal.h>
#include "utest.h"

/* Minimal timespec for nanosleep */
struct timespec {
    long tv_sec;
    long tv_nsec;
};

static volatile int sig_count = 0;

/* Signal handler that performs float operations — exercises FPU in handler.
 * On M33 this triggers the extended exception frame path. */
static void float_handler(int sig)
{
    (void)sig;
    /* Use floats inside the handler — this will activate FPU lazy stacking
     * on M33 and allocate s0-s15 on the exception frame. */
    volatile float a = 100.0f;
    volatile float b = 200.0f;
    volatile float c = a + b;
    /* Prevent the compiler from optimizing away the float ops */
    if (c < 299.0f)
        _exit(99);  /* should never happen */
    sig_count++;
}

/* Compare floats with tolerance (no fabsf — avoid libm dependency) */
static int float_eq(float a, float b)
{
    float diff = a - b;
    if (diff < 0.0f) diff = -diff;
    return diff < 0.01f;
}

int main(void)
{
    /* 1. Float registers survive signal delivery + return */
    volatile float x = 3.14f;
    volatile float y = 2.72f;
    volatile float sum = x + y;

    signal(10 /* SIGUSR1 */, float_handler);
    kill(getpid(), 10);

    UT_ASSERT(sig_count == 1, "handler called once");
    UT_ASSERT(float_eq(sum, 5.86f), "float sum preserved after signal");
    UT_ASSERT(float_eq(x, 3.14f), "float x preserved after signal");
    UT_ASSERT(float_eq(y, 2.72f), "float y preserved after signal");

    /* 2. Float multiply survives signal with float-using handler */
    volatile float p = 6.0f;
    volatile float q = 7.0f;
    volatile float prod = p * q;

    kill(getpid(), 10);

    UT_ASSERT(sig_count == 2, "handler called twice");
    UT_ASSERT(float_eq(prod, 42.0f), "float product preserved after signal");

    /* 3. Float accumulate across multiple signals */
    volatile float acc = 1.0f;
    acc = acc + 1.0f;
    kill(getpid(), 10);
    acc = acc + 1.0f;
    kill(getpid(), 10);
    UT_ASSERT(sig_count == 4, "handler called four times");
    UT_ASSERT(float_eq(acc, 3.0f), "float accumulate preserved across signals");

    UT_SUMMARY("test_signal_float");
}
