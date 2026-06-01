/*
 * test_tmpfs.c — mkdir, unlink, rmdir on /tmp (tmpfs)
 */

#include "common/time.h"
#include "utest.h"

#define TMPFS_STRESS_ROUNDS 16

#define CHILD_OK 0
#define CHILD_BAD_ARGS 11
#define CHILD_READY_WRITE_FAIL 12
#define CHILD_START_READ_FAIL 13
#define CHILD_STRESS_FAIL 14

/* Helper: compare two strings */
static int streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int parse_fd(const char *s)
{
    int v = 0;
    if (!s || !*s) return -1;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

static void fd_to_arg(int fd, char out[4])
{
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

static void short_sleep(void)
{
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000;
    nanosleep(&ts, (void *)0);
}

static int churn_tmpfs_file(const char *old_path, const char *new_path,
                            char marker)
{
    for (int i = 0; i < TMPFS_STRESS_ROUNDS; i++) {
        int fd = open(old_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return -1;
        if (write(fd, &marker, 1) != 1) return -1;
        if (close(fd) < 0) return -1;

        if (rename(old_path, new_path) < 0) return -1;

        fd = open(new_path, O_RDONLY, 0);
        if (fd < 0) return -1;
        char ch = 0;
        if (read(fd, &ch, 1) != 1 || ch != marker) return -1;
        if (close(fd) < 0) return -1;

        if (unlink(new_path) < 0) return -1;
        short_sleep();
    }
    return 0;
}

static int tmpfs_worker_child(int argc, char **argv)
{
    if (argc < 6) return CHILD_BAD_ARGS;

    int ready_r = parse_fd(argv[2]);
    int ready_w = parse_fd(argv[3]);
    int start_r = parse_fd(argv[4]);
    int start_w = parse_fd(argv[5]);
    if (ready_r < 0 || ready_w < 0 || start_r < 0 || start_w < 0)
        return CHILD_BAD_ARGS;

    close(ready_r);
    close(start_w);

    char ch = 'R';
    if (write(ready_w, &ch, 1) != 1) return CHILD_READY_WRITE_FAIL;
    close(ready_w);

    if (read(start_r, &ch, 1) != 1 || ch != 'S') return CHILD_START_READ_FAIL;
    close(start_r);

    return churn_tmpfs_file("/tmp/stress_child_a", "/tmp/stress_child_b", 'C')
               ? CHILD_STRESS_FAIL
               : CHILD_OK;
}

static void run_tmpfs_stress(void)
{
    int ready[2];
    int start[2];
    ready[0] = ready[1] = start[0] = start[1] = -1;

    int ready_rc = pipe(ready);
    UT_ASSERT_EQ(ready_rc, 0);
    int start_rc = pipe(start);
    UT_ASSERT_EQ(start_rc, 0);

    if (ready_rc == 0 && start_rc == 0) {
        char ready_r_arg[4], ready_w_arg[4];
        char start_r_arg[4], start_w_arg[4];
        fd_to_arg(ready[0], ready_r_arg);
        fd_to_arg(ready[1], ready_w_arg);
        fd_to_arg(start[0], start_r_arg);
        fd_to_arg(start[1], start_w_arg);
        char *child_argv[] = {
            (char *)"/bin/test_tmpfs", (char *)"--tmpfs-worker",
            ready_r_arg, ready_w_arg, start_r_arg, start_w_arg, (char *)0};

        pid_t pid = vfork();
        if (pid == 0) {
            execve("/bin/test_tmpfs", child_argv, (void *)0);
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

        UT_ASSERT(handshake_ok, "tmpfs stress worker rendezvous");
        UT_ASSERT_EQ(churn_tmpfs_file("/tmp/stress_parent_a",
                                      "/tmp/stress_parent_b", 'P'), 0);

        int status = 0;
        UT_ASSERT_EQ(waitpid(pid, &status, 0), pid);
        UT_ASSERT(WIFEXITED(status), "tmpfs stress worker exited normally");
        UT_ASSERT_EQ(WEXITSTATUS(status), CHILD_OK);
        UT_ASSERT(access("/tmp/stress_parent_a", F_OK) < 0,
                  "parent tmpfs stress source cleaned up");
        UT_ASSERT(access("/tmp/stress_parent_b", F_OK) < 0,
                  "parent tmpfs stress target cleaned up");
        UT_ASSERT(access("/tmp/stress_child_a", F_OK) < 0,
                  "child tmpfs stress source cleaned up");
        UT_ASSERT(access("/tmp/stress_child_b", F_OK) < 0,
                  "child tmpfs stress target cleaned up");
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && streq(argv[1], "--tmpfs-worker"))
        return tmpfs_worker_child(argc, argv);

    /* 1. /tmp should exist and be a directory */
    struct stat st;
    int ret = stat("/tmp", &st);
    UT_ASSERT_EQ(ret, 0);
    UT_ASSERT(S_ISDIR(st.st_mode), "/tmp is directory");

    /* 2. Create a file in /tmp: open + write + close */
    int fd = open("/tmp/test_tmpfs_file", O_WRONLY | O_CREAT, 0644);
    UT_ASSERT(fd >= 0, "create /tmp/test_tmpfs_file");
    if (fd >= 0) {
        write(fd, "HELLO", 5);
        close(fd);
    }

    /* 3. stat the created file */
    ret = stat("/tmp/test_tmpfs_file", &st);
    UT_ASSERT_EQ(ret, 0);
    UT_ASSERT(S_ISREG(st.st_mode), "tmpfs file is regular");
    UT_ASSERT_EQ((int)st.st_size, 5);

    /* 4. Read back the file */
    fd = open("/tmp/test_tmpfs_file", O_RDONLY, 0);
    UT_ASSERT(fd >= 0, "open tmpfs file for read");
    if (fd >= 0) {
        char buf[8];
        int i;
        for (i = 0; i < 8; i++) buf[i] = 0;
        ssize_t n = read(fd, buf, 8);
        UT_ASSERT_EQ(n, 5);
        UT_ASSERT(streq(buf, "HELLO"), "tmpfs read matches write");
        close(fd);
    }

    /* 5. unlink the file */
    ret = unlink("/tmp/test_tmpfs_file");
    UT_ASSERT_EQ(ret, 0);

    /* 6. stat after unlink should fail */
    ret = stat("/tmp/test_tmpfs_file", &st);
    UT_ASSERT(ret < 0, "stat after unlink fails");

    /* 7. mkdir in /tmp */
    ret = mkdir("/tmp/test_subdir", 0755);
    UT_ASSERT_EQ(ret, 0);

    /* 8. stat the new directory */
    ret = stat("/tmp/test_subdir", &st);
    UT_ASSERT_EQ(ret, 0);
    UT_ASSERT(S_ISDIR(st.st_mode), "mkdir created directory");

    /* 9. rmdir */
    ret = rmdir("/tmp/test_subdir");
    UT_ASSERT_EQ(ret, 0);

    /* 10. stat after rmdir should fail */
    ret = stat("/tmp/test_subdir", &st);
    UT_ASSERT(ret < 0, "stat after rmdir fails");

    /* 11. unlink nonexistent should fail */
    ret = unlink("/tmp/no_such_file");
    UT_ASSERT(ret < 0, "unlink nonexistent fails");

    /* 12. rmdir nonexistent should fail */
    ret = rmdir("/tmp/no_such_dir");
    UT_ASSERT(ret < 0, "rmdir nonexistent fails");

    /* 13. Large write + read (4096 bytes, full tmpfs file capacity).
     * Keep both full-page buffers in BSS so this test does not consume the
     * entire user stack page.  The next test covers page-boundary walking. */
    {
        static char wbuf[4096];
        int i;
        for (i = 0; i < (int)sizeof(wbuf); i++)
            wbuf[i] = (char)(i & 0x7f);

        fd = open("/tmp/test_large", O_WRONLY | O_CREAT, 0644);
        UT_ASSERT(fd >= 0, "create large file");
        if (fd >= 0) {
            ssize_t nw = write(fd, wbuf, sizeof(wbuf));
            UT_ASSERT_EQ(nw, (int)sizeof(wbuf));
            close(fd);
        }

        /* Read back in one call.  rbuf is static so it lives in BSS,
         * not on the 4 KB user stack page. */
        static char rbuf[4096];
        for (i = 0; i < (int)sizeof(rbuf); i++)
            rbuf[i] = 0;

        fd = open("/tmp/test_large", O_RDONLY, 0);
        UT_ASSERT(fd >= 0, "open large file for read");
        if (fd >= 0) {
            ssize_t nr = read(fd, rbuf, sizeof(rbuf));
            UT_ASSERT_EQ(nr, (int)sizeof(rbuf));

            /* Verify content */
            int mismatch = 0;
            for (i = 0; i < (int)sizeof(rbuf); i++) {
                if (rbuf[i] != (char)(i & 0x7f)) { mismatch = i + 1; break; }
            }
            UT_ASSERT_EQ(mismatch, 0);
            close(fd);
        }

        unlink("/tmp/test_large");
    }

    /* 14. Read straddling a page boundary in the user buffer.
     * Allocate two adjacent buffers so the second starts near the
     * end of a stack frame, making it likely that part of buf[]
     * falls in one page and part in the next. We force this by
     * using a gap variable to push buf to a known offset. */
    {
        static char gap[3900]; /* push buf near page boundary; static keeps
                                * it out of the 4 KB user stack page. */
        char buf[256];
        int i;
        gap[0] = 0; /* prevent optimizer from removing gap */
        (void)gap;

        /* Write a pattern */
        for (i = 0; i < (int)sizeof(buf); i++)
            buf[i] = (char)(0x41 + (i % 26));

        fd = open("/tmp/test_straddle", O_WRONLY | O_CREAT, 0644);
        UT_ASSERT(fd >= 0, "create straddle file");
        if (fd >= 0) {
            ssize_t nw = write(fd, buf, sizeof(buf));
            UT_ASSERT_EQ(nw, (int)sizeof(buf));
            close(fd);
        }

        /* Clear and read back */
        for (i = 0; i < (int)sizeof(buf); i++)
            buf[i] = 0;

        fd = open("/tmp/test_straddle", O_RDONLY, 0);
        UT_ASSERT(fd >= 0, "open straddle file for read");
        if (fd >= 0) {
            ssize_t nr = read(fd, buf, sizeof(buf));
            UT_ASSERT_EQ(nr, (int)sizeof(buf));

            int mismatch = 0;
            for (i = 0; i < (int)sizeof(buf); i++) {
                if (buf[i] != (char)(0x41 + (i % 26))) { mismatch = i + 1; break; }
            }
            UT_ASSERT_EQ(mismatch, 0);
            close(fd);
        }

        unlink("/tmp/test_straddle");
    }

    /* 15. Relative paths resolve against cwd for file-creation syscalls.
     * Regression guard for a bug where vfs_lookup_parent rejected any
     * non-absolute path with EINVAL while vfs_lookup_flags already
     * handled cwd — so `mkdir /tmp/x` worked but `mkdir x` from /tmp
     * did not.  Covers mkdir, rmdir, open(O_CREAT), unlink. */
    {
        ret = chdir("/tmp");
        UT_ASSERT_EQ(ret, 0);

        /* mkdir with a bare name lands under /tmp */
        ret = mkdir("test_rel_dir", 0755);
        UT_ASSERT_EQ(ret, 0);
        ret = stat("/tmp/test_rel_dir", &st);
        UT_ASSERT_EQ(ret, 0);
        UT_ASSERT(S_ISDIR(st.st_mode), "relative mkdir created /tmp/test_rel_dir");
        ret = rmdir("test_rel_dir");
        UT_ASSERT_EQ(ret, 0);

        /* open(O_CREAT) with a bare name lands under /tmp */
        fd = open("test_rel_file", O_WRONLY | O_CREAT, 0644);
        UT_ASSERT(fd >= 0, "relative open(O_CREAT)");
        if (fd >= 0) close(fd);
        ret = stat("/tmp/test_rel_file", &st);
        UT_ASSERT_EQ(ret, 0);
        UT_ASSERT(S_ISREG(st.st_mode), "relative creat made /tmp/test_rel_file");
        ret = unlink("test_rel_file");
        UT_ASSERT_EQ(ret, 0);

        /* Leave cwd where other tests expect it. */
        chdir("/");
    }

    /* 16. tmpfs stamps mtime/ctime on create and write.  The kernel
     * derives stat time from the same clock as clock_gettime(CLOCK_REALTIME),
     * so a file's mtime must land between the readings taken on either side
     * of the syscall that stamped it. */
    {
        long ts_before[2], ts_after[2];
        clock_gettime(0 /* CLOCK_REALTIME */, ts_before);

        fd = open("/tmp/test_mtime", O_WRONLY | O_CREAT, 0644);
        UT_ASSERT(fd >= 0, "create /tmp/test_mtime");
        if (fd >= 0) {
            write(fd, "x", 1);
            close(fd);
        }

        clock_gettime(0, ts_after);

        ret = stat("/tmp/test_mtime", &st);
        UT_ASSERT_EQ(ret, 0);
        UT_ASSERT(st.st_mtime >= (unsigned long)ts_before[0],
                  "mtime >= clock before write");
        UT_ASSERT(st.st_mtime <= (unsigned long)ts_after[0],
                  "mtime <= clock after write");
        UT_ASSERT_EQ((int)st.st_mtime, (int)st.st_ctime);

        unlink("/tmp/test_mtime");
    }

    /* 17. utimes: explicit timestamps land in mtime/atime; ctime = now. */
    {
        fd = open("/tmp/test_utimes", O_WRONLY | O_CREAT, 0644);
        UT_ASSERT(fd >= 0, "create /tmp/test_utimes");
        if (fd >= 0) close(fd);

        /* struct timeval[2] = { atime, mtime } — use a fixed 2000-01-01
         * epoch for mtime and 2005-01-01 for atime, both well above the
         * 0-or-low value that ctime will show. */
        long times[4] = {
            1104537600L, 0L,  /* 2005-01-01 atime */
             946684800L, 0L,  /* 2000-01-01 mtime */
        };
        ret = utimes("/tmp/test_utimes", times);
        UT_ASSERT_EQ(ret, 0);

        ret = stat("/tmp/test_utimes", &st);
        UT_ASSERT_EQ(ret, 0);
        UT_ASSERT_EQ((int)st.st_mtime, (int) 946684800);
        UT_ASSERT_EQ((int)st.st_atime, (int)1104537600);
        /* ctime is "now" from the kernel wallclock — must differ from
         * the 2000 stamp we set for mtime.  (Modulo PPAP's 0-epoch boot
         * time, ctime is seconds-since-boot ≪ 946684800). */
        UT_ASSERT(st.st_ctime != st.st_mtime,
                  "ctime not overridden by utimes");

        /* NULL times = stamp to now.  New mtime must differ from the
         * fixed 946684800 we just set. */
        ret = utimes("/tmp/test_utimes", 0);
        UT_ASSERT_EQ(ret, 0);
        ret = stat("/tmp/test_utimes", &st);
        UT_ASSERT_EQ(ret, 0);
        UT_ASSERT((int)st.st_mtime != (int)946684800,
                  "utimes(NULL) stamps now");

        unlink("/tmp/test_utimes");
    }

    /* 18. utimes on a filesystem that doesn't store times returns an
     * error.  /dev entries are served by devfs which has no .utimes
     * op, so any devfs path exercises the -EPERM branch regardless
     * of which target mounts what as rootfs. */
    {
        ret = utimes("/dev/null", 0);
        UT_ASSERT(ret < 0, "utimes on devfs returns error");
    }

    /* 19. chmod: replace permission bits, preserve file type, bump
     * ctime.  Verifies the SYS_CHMOD un-stub. */
    {
        fd = open("/tmp/test_chmod", O_WRONLY | O_CREAT, 0644);
        UT_ASSERT(fd >= 0, "create /tmp/test_chmod");
        if (fd >= 0) close(fd);

        ret = stat("/tmp/test_chmod", &st);
        UT_ASSERT_EQ(ret, 0);
        UT_ASSERT(S_ISREG(st.st_mode), "type before chmod is regular");
        UT_ASSERT_EQ((int)(st.st_mode & 07777), 0644);

        ret = chmod("/tmp/test_chmod", 0700);
        UT_ASSERT_EQ(ret, 0);

        ret = stat("/tmp/test_chmod", &st);
        UT_ASSERT_EQ(ret, 0);
        UT_ASSERT(S_ISREG(st.st_mode), "type after chmod still regular");
        UT_ASSERT_EQ((int)(st.st_mode & 07777), 0700);

        unlink("/tmp/test_chmod");
    }

    /* 20. chmod on a filesystem that has no permission model returns
     * an error.  Same devfs reasoning as the utimes test above. */
    {
        ret = chmod("/dev/null", 0700);
        UT_ASSERT(ret < 0, "chmod on devfs returns error");
    }

    /* 21. Parent and exec'd child churn separate tmpfs metadata concurrently. */
    run_tmpfs_stress();

    UT_SUMMARY("test_tmpfs");
}
