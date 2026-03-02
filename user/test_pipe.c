/*
 * test_pipe.c — pipe + dup2 + cross-process I/O
 */

#include "utest.h"

int main(void)
{
    /* 1. Basic pipe write+read */
    int fds[2];
    int ret = pipe(fds);
    UT_ASSERT_EQ(ret, 0);
    UT_ASSERT(fds[0] >= 0, "pipe read fd valid");
    UT_ASSERT(fds[1] >= 0, "pipe write fd valid");
    UT_ASSERT(fds[0] != fds[1], "pipe fds distinct");

    write(fds[1], "PIPEDATA", 8);
    char buf[16];
    int i;
    for (i = 0; i < 16; i++) buf[i] = 0;
    ssize_t n = read(fds[0], buf, 16);
    UT_ASSERT_EQ(n, 8);
    int match = 1;
    for (i = 0; i < 8; i++)
        if (buf[i] != "PIPEDATA"[i]) match = 0;
    UT_ASSERT(match, "pipe data round-trip intact");

    close(fds[0]);
    close(fds[1]);

    /* 2. Pipe across vfork + dup2 */
    int fds2[2];
    pipe(fds2);
    pid_t pid = vfork();
    if (pid == 0) {
        close(fds2[0]);
        dup2(fds2[1], 1);       /* stdout = pipe write end */
        close(fds2[1]);
        write(1, "CHILD", 5);   /* goes through pipe */
        _exit(0);
    }
    close(fds2[1]);
    char buf2[16];
    for (i = 0; i < 16; i++) buf2[i] = 0;
    ssize_t n2 = read(fds2[0], buf2, 16);
    UT_ASSERT_EQ(n2, 5);
    match = 1;
    for (i = 0; i < 5; i++)
        if (buf2[i] != "CHILD"[i]) match = 0;
    UT_ASSERT(match, "pipe across vfork: data correct");
    close(fds2[0]);
    waitpid(pid, (void *)0, 0);

    /* 3. EOF on pipe when writer closes */
    int fds3[2];
    pipe(fds3);
    close(fds3[1]);             /* close write end */
    char buf3[4];
    ssize_t n3 = read(fds3[0], buf3, 4);
    UT_ASSERT_EQ(n3, 0);       /* should return EOF */
    close(fds3[0]);

    UT_SUMMARY("test_pipe");
}
