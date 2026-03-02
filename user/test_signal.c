/*
 * test_signal.c — Signal delivery and handler execution
 *
 * Tests the full signal trampoline path:
 *   sigaction → kill → signal_check → signal_setup_frame → handler
 *   → sigreturn_trampoline → sys_sigreturn → context restore
 */

#include "utest.h"

static volatile int sig_received = 0;
static volatile int sig_number = 0;

static void handler(int sig)
{
    sig_received = 1;
    sig_number = sig;
}

int main(void)
{
    /* 1. Install handler and send signal to self */
    sigaction(10 /* SIGUSR1 */, (void *)handler, (void *)0);
    kill(getpid(), 10);
    UT_ASSERT(sig_received, "handler should have been called");
    UT_ASSERT_EQ(sig_number, 10);

    /* 2. SIG_IGN — signal should be silently ignored */
    sig_received = 0;
    sigaction(10, (void *)1 /* SIG_IGN */, (void *)0);
    kill(getpid(), 10);
    UT_ASSERT(!sig_received, "SIG_IGN should not call handler");

    /* 3. Handler is called, then execution resumes correctly */
    sig_received = 0;
    sigaction(10, (void *)handler, (void *)0);
    int before = 42;
    kill(getpid(), 10);
    int after = before;   /* should still be 42 after handler returns */
    UT_ASSERT_EQ(after, 42);
    UT_ASSERT(sig_received, "handler called on second delivery");

    UT_SUMMARY("test_signal");
}
