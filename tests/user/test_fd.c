/*
 * test_fd.c — dup, dup2, close tests
 */

#include "common/time.h"
#include "utest.h"

#define FD_STRESS_ROUNDS 16
#define OFFSET_STRESS_ROUNDS 16
#define OFFSET_STRESS_BYTES (OFFSET_STRESS_ROUNDS * 2)

#define CHILD_OK 0
#define CHILD_BAD_ARGS 11
#define CHILD_READY_WRITE_FAIL 12
#define CHILD_START_READ_FAIL 13
#define CHILD_STRESS_FAIL 14
#define CHILD_RESULT_WRITE_FAIL 15

static const char offset_pattern[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef";

static int offset_pattern_index(char ch) {
    for (int i = 0; i < OFFSET_STRESS_BYTES; i++) {
        if (offset_pattern[i] == ch) return i;
    }
    return -1;
}

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

static void short_sleep(void) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000;
    nanosleep(&ts, (void *)0);
}

static int churn_shared_file(int shared_fd) {
    for (int i = 0; i < FD_STRESS_ROUNDS; i++) {
        int dup_fd = dup(shared_fd);
        if (dup_fd < 0) return -1;
        if (close(dup_fd) < 0) return -1;

        int open_fd = open("/bin/test_fd", O_RDONLY, 0);
        if (open_fd < 0) return -1;
        if (close(open_fd) < 0) return -1;

        short_sleep();
    }
    return 0;
}

static int fd_worker_child(int argc, char **argv) {
    if (argc < 7) return CHILD_BAD_ARGS;

    int shared_fd = parse_fd(argv[2]);
    int ready_r = parse_fd(argv[3]);
    int ready_w = parse_fd(argv[4]);
    int start_r = parse_fd(argv[5]);
    int start_w = parse_fd(argv[6]);
    if (shared_fd < 0 || ready_r < 0 || ready_w < 0 || start_r < 0 ||
        start_w < 0)
        return CHILD_BAD_ARGS;

    close(ready_r);
    close(start_w);

    char ch = 'R';
    if (write(ready_w, &ch, 1) != 1) return CHILD_READY_WRITE_FAIL;
    close(ready_w);

    if (read(start_r, &ch, 1) != 1 || ch != 'S') return CHILD_START_READ_FAIL;
    close(start_r);

    return churn_shared_file(shared_fd) == 0 ? CHILD_OK : CHILD_STRESS_FAIL;
}

static int read_shared_offset(int shared_fd, char out[OFFSET_STRESS_ROUNDS]) {
    for (int i = 0; i < OFFSET_STRESS_ROUNDS; i++) {
        if (lseek(shared_fd, 0, SEEK_CUR) < 0) return -1;
        if (read(shared_fd, &out[i], 1) != 1) return -1;
        short_sleep();
    }
    return 0;
}

static int offset_worker_child(int argc, char **argv) {
    if (argc < 9) return CHILD_BAD_ARGS;

    int shared_fd = parse_fd(argv[2]);
    int ready_r = parse_fd(argv[3]);
    int ready_w = parse_fd(argv[4]);
    int start_r = parse_fd(argv[5]);
    int start_w = parse_fd(argv[6]);
    int result_r = parse_fd(argv[7]);
    int result_w = parse_fd(argv[8]);
    if (shared_fd < 0 || ready_r < 0 || ready_w < 0 || start_r < 0 ||
        start_w < 0 || result_r < 0 || result_w < 0)
        return CHILD_BAD_ARGS;

    close(ready_r);
    close(start_w);
    close(result_r);

    char ch = 'R';
    if (write(ready_w, &ch, 1) != 1) return CHILD_READY_WRITE_FAIL;
    close(ready_w);

    if (read(start_r, &ch, 1) != 1 || ch != 'S') return CHILD_START_READ_FAIL;
    close(start_r);

    char bytes[OFFSET_STRESS_ROUNDS];
    if (read_shared_offset(shared_fd, bytes) < 0) return CHILD_STRESS_FAIL;
    if (write(result_w, bytes, sizeof(bytes)) != (int)sizeof(bytes))
        return CHILD_RESULT_WRITE_FAIL;
    close(result_w);
    return CHILD_OK;
}

int main(int argc, char **argv) {
    if (argc > 1 && streq(argv[1], "--fd-worker"))
        return fd_worker_child(argc, argv);
    if (argc > 1 && streq(argv[1], "--offset-worker"))
        return offset_worker_child(argc, argv);

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

    /* 5. Parent and exec'd child concurrently churn refs to one shared file. */
    {
        int shared_fd = open("/bin/test_fd", O_RDONLY, 0);
        int ready[2];
        int start[2];
        ready[0] = ready[1] = start[0] = start[1] = -1;

        UT_ASSERT(shared_fd >= 0, "open shared file for fd stress");
        int ready_rc = pipe(ready);
        UT_ASSERT_EQ(ready_rc, 0);
        int start_rc = pipe(start);
        UT_ASSERT_EQ(start_rc, 0);

        if (shared_fd >= 0 && ready_rc == 0 && start_rc == 0) {
            char shared_arg[4], ready_r_arg[4], ready_w_arg[4];
            char start_r_arg[4], start_w_arg[4];
            fd_to_arg(shared_fd, shared_arg);
            fd_to_arg(ready[0], ready_r_arg);
            fd_to_arg(ready[1], ready_w_arg);
            fd_to_arg(start[0], start_r_arg);
            fd_to_arg(start[1], start_w_arg);
            char *child_argv[] = {
                (char *)"/bin/test_fd", (char *)"--fd-worker",
                shared_arg, ready_r_arg, ready_w_arg, start_r_arg, start_w_arg,
                (char *)0};

            pid_t pid = vfork();
            if (pid == 0) {
                execve("/bin/test_fd", child_argv, (void *)0);
                _exit(127);
            }

            close(ready[1]);
            close(start[0]);

            char ch = 0;
            int handshake_ok = read(ready[0], &ch, 1) == 1 && ch == 'R';
            close(ready[0]);
            ch = 'S';
            if (write(start[1], &ch, 1) != 1) handshake_ok = 0;
            close(start[1]);

            UT_ASSERT(handshake_ok, "fd stress worker rendezvous");
            UT_ASSERT_EQ(churn_shared_file(shared_fd), 0);

            int status = 0;
            UT_ASSERT_EQ(waitpid(pid, &status, 0), pid);
            UT_ASSERT(WIFEXITED(status), "fd stress worker exited normally");
            UT_ASSERT_EQ(WEXITSTATUS(status), CHILD_OK);
        }

        if (shared_fd >= 0) close(shared_fd);
    }

    /* 6. Parent and child read and probe one inherited shared file offset. */
    {
        const char *path = "/tmp/test_fd_offset";
        int create_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        UT_ASSERT(create_fd >= 0, "create shared-offset file");
        if (create_fd >= 0) {
            UT_ASSERT_EQ(write(create_fd, offset_pattern, OFFSET_STRESS_BYTES),
                         OFFSET_STRESS_BYTES);
            close(create_fd);
        }

        int shared_fd = open(path, O_RDONLY, 0);
        int ready[2];
        int start[2];
        int result[2];
        ready[0] = ready[1] = start[0] = start[1] = -1;
        result[0] = result[1] = -1;

        UT_ASSERT(shared_fd >= 0, "open shared-offset file");
        int ready_rc = pipe(ready);
        UT_ASSERT_EQ(ready_rc, 0);
        int start_rc = pipe(start);
        UT_ASSERT_EQ(start_rc, 0);
        int result_rc = pipe(result);
        UT_ASSERT_EQ(result_rc, 0);

        if (shared_fd >= 0 && ready_rc == 0 && start_rc == 0 &&
            result_rc == 0) {
            char shared_arg[4], ready_r_arg[4], ready_w_arg[4];
            char start_r_arg[4], start_w_arg[4];
            char result_r_arg[4], result_w_arg[4];
            fd_to_arg(shared_fd, shared_arg);
            fd_to_arg(ready[0], ready_r_arg);
            fd_to_arg(ready[1], ready_w_arg);
            fd_to_arg(start[0], start_r_arg);
            fd_to_arg(start[1], start_w_arg);
            fd_to_arg(result[0], result_r_arg);
            fd_to_arg(result[1], result_w_arg);
            char *child_argv[] = {
                (char *)"/bin/test_fd", (char *)"--offset-worker",
                shared_arg, ready_r_arg, ready_w_arg, start_r_arg, start_w_arg,
                result_r_arg, result_w_arg, (char *)0};

            pid_t pid = vfork();
            if (pid == 0) {
                execve("/bin/test_fd", child_argv, (void *)0);
                _exit(127);
            }

            close(ready[1]);
            close(start[0]);
            close(result[1]);

            char ch = 0;
            int handshake_ok = read(ready[0], &ch, 1) == 1 && ch == 'R';
            close(ready[0]);
            ch = 'S';
            if (write(start[1], &ch, 1) != 1) handshake_ok = 0;
            close(start[1]);
            UT_ASSERT(handshake_ok, "offset stress worker rendezvous");

            char parent_bytes[OFFSET_STRESS_ROUNDS];
            char child_bytes[OFFSET_STRESS_ROUNDS];
            UT_ASSERT_EQ(read_shared_offset(shared_fd, parent_bytes), 0);
            UT_ASSERT_EQ(read(result[0], child_bytes, sizeof(child_bytes)),
                         OFFSET_STRESS_ROUNDS);
            close(result[0]);

            int status = 0;
            UT_ASSERT_EQ(waitpid(pid, &status, 0), pid);
            UT_ASSERT(WIFEXITED(status), "offset stress worker exited normally");
            UT_ASSERT_EQ(WEXITSTATUS(status), CHILD_OK);
            UT_ASSERT_EQ(lseek(shared_fd, 0, SEEK_CUR), OFFSET_STRESS_BYTES);

            int seen[OFFSET_STRESS_BYTES];
            for (int i = 0; i < OFFSET_STRESS_BYTES; i++) seen[i] = 0;
            int valid = 1;
            for (int i = 0; i < OFFSET_STRESS_ROUNDS; i++) {
                int parent_idx = offset_pattern_index(parent_bytes[i]);
                int child_idx = offset_pattern_index(child_bytes[i]);
                if (parent_idx < 0 || parent_idx >= OFFSET_STRESS_BYTES)
                    valid = 0;
                else
                    seen[parent_idx]++;
                if (child_idx < 0 || child_idx >= OFFSET_STRESS_BYTES)
                    valid = 0;
                else
                    seen[child_idx]++;
            }
            for (int i = 0; i < OFFSET_STRESS_BYTES; i++) {
                if (seen[i] != 1) valid = 0;
            }
            UT_ASSERT(valid, "shared offset consumes each byte exactly once");

            UT_ASSERT_EQ(lseek(shared_fd, 0, SEEK_SET), 0);
            ch = 0;
            UT_ASSERT_EQ(read(shared_fd, &ch, 1), 1);
            UT_ASSERT(ch == offset_pattern[0], "SEEK_SET rewinds shared file");
        }

        if (shared_fd >= 0) close(shared_fd);
        unlink(path);
    }

    UT_SUMMARY("test_fd");
}
