/*
 * test_m68k_emu.c — m68k eCPU emulation userland test
 *
 * Verifies that execve() can run an m68k ELF binary on all targets:
 *   - ARM: detects EM_68K, loads via eCPU emulator, TRAP #0 → PPAP syscalls
 *   - m68k: runs natively as a regular ELF binary
 *
 * The test binary /bin/hello_m68k is a minimal m68k program that writes
 * "Hello from m68k user space!\n" to stdout and exits with code 0.
 */

#include "utest.h"

/* ── Helper: exec a binary with stdout piped, return output length ───────── */

static int run_capture(const char *path, char *buf, int bufsize)
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
    (void)status;
    return total;
}

static int run_exit_code(const char *path)
{
    pid_t pid = vfork();
    if (pid == 0) {
        execve(path, (void *)0, (void *)0);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return (status >> 8) & 0xff;
}

/* ── Inline string compare (no libc) ─────────────────────────────────────── */

static int streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* ── Tests ────────────────────────────────────────────────────────────────── */

int main(void)
{
    /* 1. hello_m68k exits with code 0 */
    {
        int code = run_exit_code("/bin/hello_m68k");
        UT_ASSERT_EQ(code, 0);
    }

    /* 2. hello_m68k prints the expected message */
    {
        char buf[64];
        int len = run_capture("/bin/hello_m68k", buf, sizeof(buf));
        UT_ASSERT(len == 28, "output length == 28");
        UT_ASSERT(streq(buf, "Hello from m68k user space!\n"),
                  "output matches expected string");
    }

    UT_SUMMARY("test_m68k_emu");
}
