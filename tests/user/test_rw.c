/*
 * test_rw.c — Read/write edge cases on stdout, files, and invalid fds
 */

#include "utest.h"

int main(void)
{
    /* 1. write to stdout succeeds */
    ssize_t n = write(1, "rw_ok\n", 6);
    UT_ASSERT(n > 0, "write to stdout succeeds");

    /* 2. write zero bytes returns 0 */
    n = write(1, "x", 0);
    UT_ASSERT_EQ(n, 0);

    /* 3. read/write with invalid fd */
    char buf[16];
    n = read(999, buf, 1);
    UT_ASSERT(n < 0, "read invalid fd fails");
    n = write(999, "X", 1);
    UT_ASSERT(n < 0, "write invalid fd fails");

    /* 4. open a file and read it */
    int fd = open("/bin/test_rw", O_RDONLY, 0);
    UT_ASSERT(fd >= 0, "open self");

    /* 5. read returns positive */
    n = read(fd, buf, 16);
    UT_ASSERT(n > 0, "read from file returns data");

    /* 6. close and then read should fail */
    close(fd);
    n = read(fd, buf, 1);
    UT_ASSERT(n < 0, "read after close fails");

    /* 7. write to duped stdout — verify dup works */
    int fd2 = dup(1);
    UT_ASSERT(fd2 >= 3, "dup(1) returns new fd >= 3");
    n = write(fd2, "dup_ok\n", 7);
    UT_ASSERT(n > 0, "write to duped stdout");
    close(fd2);

    /* 8. close already-closed fd should fail */
    int ret = close(fd);
    UT_ASSERT(ret < 0, "double close fails");

    UT_SUMMARY("test_rw");
}
