/*
 * test_pipe.c — pipe + dup2 + cross-process I/O
 *
 * Tests bail out early on pipe() failure (ENOMEM) since page allocator
 * may be exhausted after many test processes.
 */

#include "common/time.h"
#include "utest.h"

#define CHILD_OK 0
#define CHILD_BAD_ARGS 11
#define CHILD_READY_WRITE_FAIL 12
#define CHILD_READ_COUNT_FAIL 13
#define CHILD_DATA_FAIL 14
#define CHILD_MARKER_FAIL 15

#define BLOCK_WAKE_ROUNDS 16

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int parse_fd(const char *s) {
    int v = 0;
    if (!s || !*s) return -1;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

static void fd_to_arg(int fd, char out[4]) {
    if (fd >= 100) {
        out[0] = '0';
        out[1] = 0;
    } else if (fd >= 10) {
        out[0] = (char)('0' + fd / 10);
        out[1] = (char)('0' + fd % 10);
        out[2] = 0;
    } else {
        out[0] = (char)('0' + fd);
        out[1] = 0;
    }
}

static int pipe_reader_child(int argc, char **argv) {
    if (argc < 6) return CHILD_BAD_ARGS;

    int data_r = parse_fd(argv[2]);
    int data_w = parse_fd(argv[3]);
    int ready_r = parse_fd(argv[4]);
    int ready_w = parse_fd(argv[5]);
    if (data_r < 0 || data_w < 0 || ready_r < 0 || ready_w < 0)
        return CHILD_BAD_ARGS;

    close(data_w);
    close(ready_r);

    volatile uint32_t marker = 0x13579BDFu;
    for (int i = 0; i < BLOCK_WAKE_ROUNDS; i++) {
        char ready = 'R';
        if (write(ready_w, &ready, 1) != 1) return CHILD_READY_WRITE_FAIL;

        char ch = 0;
        if (read(data_r, &ch, 1) != 1) return CHILD_READ_COUNT_FAIL;
        if (ch != (char)('A' + i)) return CHILD_DATA_FAIL;
    }
    close(ready_w);
    close(data_r);

    if (marker != 0x13579BDFu) return CHILD_MARKER_FAIL;

    return CHILD_OK;
}

static void short_sleep(void) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000;
    nanosleep(&ts, (void *)0);
}

int main(int argc, char **argv) {
    if (argc > 1 && streq(argv[1], "--pipe-reader"))
        return pipe_reader_child(argc, argv);

    /* 1. Basic pipe write+read */
    int fds[2];
    fds[0] = fds[1] = -1;
    int ret = pipe(fds);
    if (ret != 0) {
        UT_SUMMARY("test_pipe");
    }
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
    {
        int fds2[2];
        fds2[0] = fds2[1] = -1;
        ret = pipe(fds2);
        if (ret != 0) {
            UT_SUMMARY("test_pipe");
        }
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
    }

    /* 3. EOF on pipe when writer closes */
    {
        int fds3[2];
        fds3[0] = fds3[1] = -1;
        ret = pipe(fds3);
        if (ret != 0) {
            UT_SUMMARY("test_pipe");
        }
        close(fds3[1]);             /* close write end */
        char buf3[4];
        ssize_t n3 = read(fds3[0], buf3, 4);
        UT_ASSERT_EQ(n3, 0);       /* should return EOF */
        close(fds3[0]);
    }

    /* 4. Repeatedly block a child reader, then wake it with one byte. */
    {
        int data[2];
        int ready[2];
        data[0] = data[1] = ready[0] = ready[1] = -1;
        ret = pipe(data);
        if (ret != 0) {
            UT_SUMMARY("test_pipe");
        }
        ret = pipe(ready);
        if (ret != 0) {
            close(data[0]);
            close(data[1]);
            UT_SUMMARY("test_pipe");
        }

        char data_r_arg[4], data_w_arg[4], ready_r_arg[4], ready_w_arg[4];
        fd_to_arg(data[0], data_r_arg);
        fd_to_arg(data[1], data_w_arg);
        fd_to_arg(ready[0], ready_r_arg);
        fd_to_arg(ready[1], ready_w_arg);
        char *child_argv[] = {
            (char *)"/bin/test_pipe", (char *)"--pipe-reader",
            data_r_arg, data_w_arg, ready_r_arg, ready_w_arg, (char *)0};

        pid_t pid = vfork();
        if (pid == 0) {
            execve("/bin/test_pipe", child_argv, (void *)0);
            _exit(127);
        }

        close(data[0]);
        close(ready[1]);

        int handshake_ok = 1;
        for (i = 0; i < BLOCK_WAKE_ROUNDS; i++) {
            char ready_ch = 0;
            ssize_t rn = read(ready[0], &ready_ch, 1);
            if (rn != 1 || ready_ch != 'R') handshake_ok = 0;

            short_sleep();

            char ch = (char)('A' + i);
            if (write(data[1], &ch, 1) != 1) handshake_ok = 0;
        }
        UT_ASSERT(handshake_ok, "blocking reader completed wakeup rounds");
        close(ready[0]);
        close(data[1]);

        int status = 0;
        UT_ASSERT_EQ(waitpid(pid, &status, 0), pid);
        UT_ASSERT(WIFEXITED(status), "blocking reader exited normally");
        UT_ASSERT_EQ(WEXITSTATUS(status), CHILD_OK);
    }

    UT_SUMMARY("test_pipe");
}
