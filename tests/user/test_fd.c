/*
 * test_fd.c — dup, dup2, close tests
 */

#include "utest.h"

int main(void)
{
    /* 1. dup2 same fd is a no-op */
    int ret2 = dup2(1, 1);
    UT_ASSERT_EQ(ret2, 1);

    /* 2. close invalid fd returns error */
    int ret = close(99);
    UT_ASSERT(ret < 0, "close(99) should fail");

    /* 3. dup on stdout */
    int fd = dup(1);
    UT_ASSERT(fd >= 3, "dup(1) returns new fd >= 3");
    close(fd);

    /* 4. dup invalid fd */
    fd = dup(99);
    UT_ASSERT(fd < 0, "dup(99) should fail");

    UT_SUMMARY("test_fd");
}
