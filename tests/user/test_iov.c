/*
 * test_iov.c — Scatter/gather I/O: writev, readv
 */

#include "utest.h"

int main(void)
{
    /* 1. writev to stdout with two segments */
    struct iovec iov[2];
    iov[0].iov_base = "HEL";
    iov[0].iov_len = 3;
    iov[1].iov_base = "LO\n";
    iov[1].iov_len = 3;
    ssize_t n = writev(1, iov, 2);
    UT_ASSERT_EQ(n, 6);

    /* 2. writev with zero-length segment */
    iov[0].iov_base = "";
    iov[0].iov_len = 0;
    iov[1].iov_base = "OK\n";
    iov[1].iov_len = 3;
    n = writev(1, iov, 2);
    UT_ASSERT_EQ(n, 3);

    /* 3. writev with single segment */
    iov[0].iov_base = "X";
    iov[0].iov_len = 1;
    n = writev(1, iov, 1);
    UT_ASSERT_EQ(n, 1);

    /* 4. writev with iovcnt=0 should fail */
    n = writev(1, iov, 0);
    UT_ASSERT(n < 0, "writev iovcnt=0 fails");

    /* 5. writev to invalid fd */
    n = writev(999, iov, 1);
    UT_ASSERT(n < 0, "writev bad fd fails");

    /* 6. readv from a file */
    int fd = open("/bin/test_iov", O_RDONLY, 0);
    UT_ASSERT(fd >= 0, "open self for readv");
    if (fd >= 0) {
        char buf1[4];
        char buf2[4];
        int i;
        for (i = 0; i < 4; i++) buf1[i] = buf2[i] = 0;
        iov[0].iov_base = buf1;
        iov[0].iov_len = 4;
        iov[1].iov_base = buf2;
        iov[1].iov_len = 4;
        n = readv(fd, iov, 2);
        UT_ASSERT_EQ(n, 8);
        close(fd);
    }

    UT_SUMMARY("test_iov");
}
