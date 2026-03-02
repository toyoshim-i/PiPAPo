/*
 * test_fd.c — dup, dup2, close tests
 */

#include "utest.h"

int main(void)
{
    /* 1. dup2 redirects stdout through pipe */
    int fds[2];
    pipe(fds);
    int saved_stdout = dup(1);          /* save original stdout */
    dup2(fds[1], 1);                    /* redirect stdout to pipe */
    write(1, "DUP", 3);
    dup2(saved_stdout, 1);              /* restore stdout */
    close(fds[1]);
    char buf[8];
    int i;
    for (i = 0; i < 8; i++) buf[i] = 0;
    ssize_t n = read(fds[0], buf, 8);
    UT_ASSERT_EQ(n, 3);
    UT_ASSERT(buf[0] == 'D' && buf[1] == 'U' && buf[2] == 'P',
              "dup2 redirect should route write through pipe");
    close(fds[0]);
    close(saved_stdout);

    /* 2. close invalid fd returns error */
    int ret = close(99);
    UT_ASSERT(ret < 0, "close(99) should fail");

    /* 3. dup2 same fd is a no-op */
    int ret2 = dup2(1, 1);
    UT_ASSERT_EQ(ret2, 1);

    UT_SUMMARY("test_fd");
}
