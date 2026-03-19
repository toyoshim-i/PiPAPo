/*
 * test_zexall.c — Run ZEXALL via the full CP/M subsystem
 *
 * Same as test_zexdoc.c but uses ZEXALL which also verifies undocumented
 * F3/F5 flag behaviour with stricter CRC checks.
 */

#include "utest.h"

/* ── Helper: copy a file ─────────────────────────────────────────────── */

static int copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY, 0);
    if (in < 0)
        return -1;
    unlink(dst);
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[512];
    int n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        write(out, buf, n);
    close(out);
    close(in);
    return 0;
}

/* ── Helper: run .COM with captured output ───────────────────────────── */

static int run_com_capture(const char *path, char *buf, int bufsize)
{
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return -1;

    pid_t pid = vfork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        close(pipefd[1]);
        execve(path, (void *)0, (void *)0);
        _exit(127);
    }

    close(pipefd[1]);

    int total = 0;
    while (total < bufsize - 1) {
        int n = read(pipefd[0], buf + total, bufsize - 1 - total);
        if (n <= 0)
            break;
        total += n;
    }
    buf[total] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    return total;
}

/* ── Inline strstr (no libc) ─────────────────────────────────────────── */

static const char *ut_strstr(const char *haystack, const char *needle)
{
    if (!needle[0])
        return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n)
            return haystack;
    }
    return (void *)0;
}

/* ── Test ─────────────────────────────────────────────────────────────── */

static void test_zexall(void)
{
    /* Copy zexall.com to /tmp/ */
    int rc = copy_file("/lib/zexall.com", "/tmp/zexall.com");
    UT_ASSERT(rc == 0, "copy zexall.com to /tmp");
    if (rc != 0)
        return;

    /* Run ZEXALL — output can be very large (~30 KB). */
    static char buf[65536];
    int n = run_com_capture("/tmp/zexall.com", buf, sizeof(buf));
    unlink("/tmp/zexall.com");

    UT_ASSERT(n > 0, "zexall produced output");
    if (n <= 0)
        return;

    /* Count OK and ERROR occurrences */
    int ok_count = 0;
    int error_count = 0;
    const char *p = buf;
    while ((p = ut_strstr(p, "OK")) != (void *)0) {
        ok_count++;
        p += 2;
    }
    p = buf;
    while ((p = ut_strstr(p, "ERROR")) != (void *)0) {
        error_count++;
        p += 5;
    }

    /* Report */
    UT_PRINT("  zexall: ");
    ut_print_int(ok_count);
    UT_PRINT(" OK, ");
    ut_print_int(error_count);
    UT_PRINT(" ERROR\n");

    UT_ASSERT(error_count == 0, "zexall: no errors");
    UT_ASSERT(ok_count >= 60, "zexall: sufficient test groups passed");
}

int main(void)
{
    test_zexall();
    UT_SUMMARY("test_zexall");
}
