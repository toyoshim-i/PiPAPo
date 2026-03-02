/*
 * runtests.c — On-target test runner for PPAP
 *
 * Sequentially vfork + execve each test binary, collect exit statuses.
 * Prints summary and exits with 0 (all passed) or 1 (failures).
 */

#include "syscall.h"

static void print(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

static void print_int(int v)
{
    if (v == 0) { write(1, "0", 1); return; }
    static const int powers[] = {1000000000,100000000,10000000,1000000,
                                 100000,10000,1000,100,10,1};
    int started = 0;
    int p;
    for (p = 0; p < 10; p++) {
        int d = 0;
        while (v >= powers[p]) { v -= powers[p]; d++; }
        if (d || started) {
            char c = '0' + d;
            write(1, &c, 1);
            started = 1;
        }
    }
}

int main(void)
{
    /* Initialize the test list at runtime — PIC binaries don't support
     * static pointer arrays because do_execve only relocates GOT entries,
     * not initialized data pointers.  Runtime assignment uses GOT-resolved
     * addresses which are correctly relocated. */
    const char *tests[8];
    tests[0] = "/bin/test_exec";
    tests[1] = "/bin/test_vfork";
    tests[2] = "/bin/test_pipe";
    tests[3] = "/bin/test_brk";
    tests[4] = "/bin/test_fd";
    tests[5] = "/bin/test_signal";
    tests[6] = (void *)0;

    print("=== PPAP on-target test suite ===\n");
    int total = 0, failed = 0;

    int i;
    for (i = 0; tests[i]; i++) {
        print("RUN   ");
        print(tests[i]);
        print("\n");

        pid_t pid = vfork();
        if (pid == 0) {
            execve(tests[i], (void *)0, (void *)0);
            _exit(127);   /* exec failed */
        }

        int status = 0;
        waitpid(pid, &status, 0);
        int code = (status >> 8) & 0xff;

        total++;
        if (code != 0) {
            failed++;
            print("FAIL  ");
            print(tests[i]);
            print(" (exit ");
            print_int(code);
            print(")\n");
        } else {
            print("PASS  ");
            print(tests[i]);
            print("\n");
        }
    }

    print("\n=== Results: ");
    print_int(total);
    print(" tests, ");
    print_int(failed);
    print(" failed ===\n");

    if (failed == 0)
        print("ALL TESTS PASSED\n");
    else
        print("SOME TESTS FAILED\n");

    return failed;
}
