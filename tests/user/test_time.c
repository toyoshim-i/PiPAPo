/*
 * test_time.c — Time syscalls: nanosleep basic behavior
 */

#include "utest.h"

/* Minimal timespec for nanosleep */
struct timespec {
    long tv_sec;
    long tv_nsec;
};

int main(void)
{
    /* 1. nanosleep with zero time returns immediately */
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    int ret = nanosleep(&ts, (void *)0);
    UT_ASSERT_EQ(ret, 0);

    /* 2. nanosleep with small duration returns 0 */
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000;   /* 1ms */
    ret = nanosleep(&ts, (void *)0);
    UT_ASSERT_EQ(ret, 0);

    /* 3. nanosleep with remainder pointer */
    struct timespec rem;
    rem.tv_sec = 99;
    rem.tv_nsec = 99;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000;   /* 1ms */
    ret = nanosleep(&ts, &rem);
    UT_ASSERT_EQ(ret, 0);

    /* 4. nanosleep with NULL req should fail */
    ret = nanosleep((void *)0, (void *)0);
    UT_ASSERT(ret < 0, "nanosleep NULL req fails");

    UT_SUMMARY("test_time");
}
