/*
 * test_musl.c — Harness for musl libc integration tests
 *
 * This test is a bare-metal binary (linked with crt0.o + syscall.o like
 * all other user tests).  It exercises musl libc by vfork+execve'ing a
 * musl-linked child binary (test_musl_child) with different argv[1]
 * values, then checks exit codes.
 *
 * Primary purpose: detect double-free and other process lifecycle bugs
 * that only manifest with musl-linked binaries (e.g. musl's _Exit()
 * calling SYS_exit_group then SYS_exit in a loop).
 */

#include "utest.h"

static void run_subtest(const char *name)
{
    pid_t pid = vfork();
    if (pid == 0) {
        char *argv[] = { "/bin/test_musl_child", (char *)name, (char *)0 };
        execve("/bin/test_musl_child", argv, (void *)0);
        _exit(127);  /* execve failed */
    }

    int status = 0;
    waitpid(pid, &status, 0);
    int code = (status >> 8) & 0xff;

    if (code == 127) {
        UT_PRINT("  SKIP  test_musl_child not found (musl not built?)\n");
        return;
    }

    /* Build assertion message at compile time for each sub-test */
    ut_total++;
    if (code != 0) {
        ut_fail++;
        UT_PRINT("  FAIL  musl child '");
        /* Print the sub-test name character by character */
        const char *p = name;
        while (*p) { write(1, p, 1); p++; }
        UT_PRINT("' exited with ");
        ut_print_int(code);
        UT_PRINT("\n");
    }
}

int main(void)
{
    /* Baseline: can a musl-linked binary exit cleanly? */
    run_subtest("exit");

    /* stdio: printf exercises buffered I/O + write syscall */
    run_subtest("printf");

    /* heap: malloc + free */
    run_subtest("malloc");

    /* string functions */
    run_subtest("string");

    UT_SUMMARY("test_musl");
}
