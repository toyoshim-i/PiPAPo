/*
 * test_sleep_intr.c — User-space test for vfork/waitpid and exit status
 *
 * Verifies the foundational process lifecycle that sleep+signal interruption
 * depends on:
 *   1. vfork + child _exit(42) → parent sees exit code 42
 *   2. vfork + child _exit(0) → parent sees exit code 0
 *   3. Signal self-delivery: kill(getpid(), SIGUSR1) with handler installed
 *
 * Full sleep+Ctrl-C testing requires QEMU serial input injection (out of
 * scope); the kernel integration test covers signal-matching logic directly.
 */

#include "utest.h"

static volatile int sig_flag = 0;

static void usr1_handler(int sig)
{
    (void)sig;
    sig_flag = 1;
}

int main(void)
{
    /* 1. vfork + child exits with code 42 */
    {
        pid_t pid = vfork();
        if (pid == 0) {
            _exit(42);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        int code = (status >> 8) & 0xff;
        UT_ASSERT_EQ(code, 42);
    }

    /* 2. vfork + child exits with code 0 */
    {
        pid_t pid = vfork();
        if (pid == 0) {
            _exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        int code = (status >> 8) & 0xff;
        UT_ASSERT_EQ(code, 0);
    }

    /* 3. Signal self-delivery — install handler, signal self, check flag */
    {
        sig_flag = 0;
        sigaction(10 /* SIGUSR1 */, (void *)usr1_handler, (void *)0);
        kill(getpid(), 10);
        UT_ASSERT(sig_flag == 1, "SIGUSR1 handler should run");
    }

    UT_SUMMARY("test_sleep_intr");
}
