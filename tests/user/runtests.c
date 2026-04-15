/*
 * runtests.c — On-target test runner for PPAP
 *
 * Features:
 *   - Per-test flags: ENABLED / DISABLED / FLAKY / SLOW
 *     DISABLED tests are always skipped.
 *     FLAKY tests are skipped unless /etc/test_run_flaky exists.
 *     SLOW tests are skipped unless /etc/test_run_slow exists.
 *   - Filter: if /etc/test_filter exists, only tests whose path contains
 *     that substring are run.
 *   - Timestamps: every log line is prefixed with [T+S.CC] (elapsed seconds
 *     and centiseconds since the runner started; 10 ms resolution).
 *   - Per-test elapsed time shown in PASS/FAIL lines.
 *
 * Sequentially vfork + execve each test binary, collect exit statuses.
 * Prints summary and exits with 0 (all passed) or 1 (failures).
 */

#include "syscall.h"

/* ── Test flags ──────────────────────────────────────────────────────────── */

#define TEST_ENABLED  0   /* run normally                                    */
#define TEST_DISABLED 1   /* always skip (not yet ready / platform-specific) */
#define TEST_FLAKY    2   /* skip by default; run with /etc/test_run_flaky   */
#define TEST_SLOW     3   /* skip by default; run with /etc/test_run_slow    */

typedef struct { const char *path; int flags; } test_entry_t;

/* ── String helpers ──────────────────────────────────────────────────────── */

static void print(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

#if !defined(__ia16__)
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
#endif

static void print_int(int v)
{
#if defined(__ia16__)
    unsigned int x;
    char buf[6];
    int n = 0;

    if (v < 0) {
        write(1, "-", 1);
        x = (unsigned int)(-v);
    } else {
        x = (unsigned int)v;
    }

    do {
        unsigned int q = 0;

        while (x >= 10u) {
            x -= 10u;
            q++;
        }
        buf[n++] = (char)('0' + x);
        x = q;
    } while (x > 0u);

    while (n > 0) {
        char c = buf[--n];
        write(1, &c, 1);
    }
#else
    if (v == 0) { write(1, "0", 1); return; }
    static const long powers[] = {1000000000L,100000000L,10000000L,1000000L,
                                  100000L,10000L,1000L,100L,10L,1L};
    long rem = v;
    int started = 0, p;
    for (p = 0; p < 10; p++) {
        int d = 0;
        while (rem >= powers[p]) { rem -= powers[p]; d++; }
        if (d || started) {
            char c = '0' + d;
            write(1, &c, 1);
            started = 1;
        }
    }
#endif
}

/* Print two-digit decimal 00-99 without division (M0+, no libgcc) */
#if !defined(__ia16__)
static void print_dd(int v)
{
    int tens = 0;
    while (v >= 10) { v -= 10; tens++; }
    char buf[2];
    buf[0] = '0' + tens;
    buf[1] = '0' + v;
    write(1, buf, 2);
}
#endif

/* ── Timestamps ──────────────────────────────────────────────────────────── */

#if !defined(__ia16__)
struct rt_ts { long tv_sec; long tv_nsec; };

static struct rt_ts g_start;

static void get_time(struct rt_ts *ts)
{
    clock_gettime(1 /* CLOCK_MONOTONIC */, ts);
}

/* Convert tv_nsec (multiples of 10_000_000 on PPAP) to centiseconds 0-99 */
static int nsec_to_cs(long ns)
{
    int cs = 0;
    while (ns >= 10000000L) { ns -= 10000000L; cs++; }
    return cs;
}

/* Print "[T+S.CC] " prefix showing elapsed time since g_start */
static void print_ts(void)
{
    struct rt_ts now;
    get_time(&now);
    long sec = now.tv_sec - g_start.tv_sec;
    long ns  = now.tv_nsec - g_start.tv_nsec;
    if (ns < 0) { sec--; ns += 1000000000L; }
    write(1, "[T+", 3);
    print_int((int)sec);
    write(1, ".", 1);
    print_dd(nsec_to_cs(ns));
    write(1, "] ", 2);
}

/* Print "S.CCs" elapsed between two timestamps */
static void print_elapsed(const struct rt_ts *t0, const struct rt_ts *t1)
{
    long sec = t1->tv_sec - t0->tv_sec;
    long ns  = t1->tv_nsec - t0->tv_nsec;
    if (ns < 0) { sec--; ns += 1000000000L; }
    print_int((int)sec);
    write(1, ".", 1);
    print_dd(nsec_to_cs(ns));
    write(1, "s", 1);
}

/* ── Filter ──────────────────────────────────────────────────────────────── */

static char g_filter[64];
static int  g_filter_len;

static void read_filter(void)
{
    int fd = open("/etc/test_filter", O_RDONLY, 0);
    if (fd < 0) return;
    int n = read(fd, g_filter, (int)sizeof(g_filter) - 1);
    close(fd);
    if (n <= 0) return;
    while (n > 0 && (g_filter[n-1] == '\n' || g_filter[n-1] == '\r')) n--;
    g_filter[n] = '\0';
    g_filter_len = n;
}

static int test_matches(const char *path)
{
    if (g_filter_len == 0) return 1;
    int plen = slen(path);
    int i;
    for (i = 0; i <= plen - g_filter_len; i++) {
        int j;
        for (j = 0; j < g_filter_len; j++)
            if (path[i+j] != g_filter[j]) break;
        if (j == g_filter_len) return 1;
    }
    return 0;
}

/* ── Flaky opt-in ────────────────────────────────────────────────────────── */

static int g_run_flaky;
static int g_run_slow;

static void check_run_flaky(void)
{
    int fd = open("/etc/test_run_flaky", O_RDONLY, 0);
    if (fd >= 0) { close(fd); g_run_flaky = 1; }
}

static void check_run_slow(void)
{
    int fd = open("/etc/test_run_slow", O_RDONLY, 0);
    if (fd >= 0) { close(fd); g_run_slow = 1; }
}
#endif

/* ── Main ────────────────────────────────────────────────────────────────── */

#if defined(__ia16__)
static int g_total;
static int g_passed;
static int g_failed;
static int g_skipped;

static int run_test(const char *path)
{
    pid_t pid;
    int status = 0;

    print("RUN   ");
    print(path);
    print("\n");

    pid = vfork();
    if (pid == 0) {
        execve(path, (void *)0, (void *)0);
        _exit(127);
    }

    waitpid(pid, &status, 0);
    return (status >> 8) & 0xff;
}

static void record_test(const char *path, int flags)
{
    if (flags == TEST_DISABLED) {
        g_skipped++;
        print("SKIP  ");
        print(path);
        print("  (disabled)\n");
        return;
    }

    int code = run_test(path);

    g_total++;

    if (code == 0) {
        g_passed++;
        print("PASS  ");
    } else {
        g_failed++;
        print("FAIL  ");
    }
    print(path);
    print("\n");
}

int main(void)
{
    g_total = 0;
    g_passed = 0;
    g_failed = 0;
    g_skipped = 0;

    /* On pcxt, stdout is the VGA console (invisible to QEMU serial
     * capture).  Redirect fd 1 to /dev/ttyS0 so test output appears
     * on the serial port where the test runner can see it. */
    {
        int sfd = open("/dev/ttyS0", 1 /* O_WRONLY */, 0);
        if (sfd >= 0) {
            close(1);
            dup(sfd);
            close(sfd);
        }
    }

    print("=== PPAP on-target test suite ===\n");
    record_test("/bin/test_exec",  TEST_ENABLED);
    record_test("/bin/test_vfork", TEST_ENABLED);
    record_test("/bin/test_fs",    TEST_ENABLED);
    record_test("/bin/test_rw",    TEST_ENABLED);
    record_test("/bin/test_tmpfs", TEST_DISABLED);
    record_test("/bin/test_ufs",   TEST_ENABLED);
    record_test("/bin/test_msdos", TEST_ENABLED);

    print("\n=== Results: ");
    print_int(g_total);
    print(" run, ");
    print_int(g_passed);
    print(" passed, ");
    print_int(g_failed);
    print(" failed, ");
    print_int(g_skipped);
    print(" skipped ===\n");

    if (g_failed == 0)
        print("ALL TESTS PASSED\n");
    else
        print("SOME TESTS FAILED\n");

    return g_failed;
}
#else
int main(void)
{
    /* Initialize the test list at runtime — PIC binaries don't support
     * static pointer arrays because execve only relocates GOT entries,
     * not initialized data pointers.  Runtime assignment uses GOT-resolved
     * addresses which are correctly relocated. */
    test_entry_t tests[30];
    int t = 0;
/* Known qemu_rv32 failures are disabled until fixed. */
#if defined(__riscv)
    tests[t++] = (test_entry_t){ "/bin/test_exec",       TEST_DISABLED };
    tests[t++] = (test_entry_t){ "/bin/test_elf",        TEST_DISABLED };
#else
    tests[t++] = (test_entry_t){ "/bin/test_exec",       TEST_ENABLED  };
    tests[t++] = (test_entry_t){ "/bin/test_elf",        TEST_ENABLED  };
#endif
    tests[t++] = (test_entry_t){ "/bin/test_vfork",      TEST_ENABLED  };
#if defined(__riscv)
    tests[t++] = (test_entry_t){ "/bin/test_fault",      TEST_DISABLED }; /* TODO: RISC-V fault handler */
#else
    tests[t++] = (test_entry_t){ "/bin/test_fault",      TEST_ENABLED  };
#endif
    tests[t++] = (test_entry_t){ "/bin/test_pipe",       TEST_ENABLED  };
/* TODO: qemu_rv32 brk base address mismatch */
#if defined(__riscv)
    tests[t++] = (test_entry_t){ "/bin/test_brk",        TEST_DISABLED };
#else
    tests[t++] = (test_entry_t){ "/bin/test_brk",        TEST_ENABLED  };
#endif
    tests[t++] = (test_entry_t){ "/bin/test_fd",         TEST_ENABLED  };
/* TODO: qemu_rv32 signal delivery issues */
#if defined(__riscv)
    tests[t++] = (test_entry_t){ "/bin/test_signal",     TEST_DISABLED };
#else
    tests[t++] = (test_entry_t){ "/bin/test_signal",     TEST_ENABLED  };
#endif
    tests[t++] = (test_entry_t){ "/bin/test_poll",       TEST_ENABLED  };
/* TODO: qemu_rv32 signal/wait semantics */
#if defined(__riscv)
    tests[t++] = (test_entry_t){ "/bin/test_sleep_intr", TEST_DISABLED };
    tests[t++] = (test_entry_t){ "/bin/test_orphan",     TEST_DISABLED };
#else
    tests[t++] = (test_entry_t){ "/bin/test_sleep_intr", TEST_ENABLED  };
    tests[t++] = (test_entry_t){ "/bin/test_orphan",     TEST_ENABLED  };
#endif
    tests[t++] = (test_entry_t){ "/bin/test_id",         TEST_ENABLED  };
/* TODO: qemu_rv32 fs result reporting mismatch */
#if defined(__riscv)
    tests[t++] = (test_entry_t){ "/bin/test_fs",         TEST_DISABLED };
#else
    tests[t++] = (test_entry_t){ "/bin/test_fs",         TEST_ENABLED  };
#endif
    tests[t++] = (test_entry_t){ "/bin/test_rw",         TEST_ENABLED  };
/* TODO: qemu_rv32 clock_gettime EINVAL path */
#if defined(__riscv)
    tests[t++] = (test_entry_t){ "/bin/test_time",       TEST_DISABLED };
#else
    tests[t++] = (test_entry_t){ "/bin/test_time",       TEST_ENABLED  };
#endif
    tests[t++] = (test_entry_t){ "/bin/test_iov",        TEST_ENABLED  };
    tests[t++] = (test_entry_t){ "/bin/test_stat",       TEST_ENABLED  };
    tests[t++] = (test_entry_t){ "/bin/test_tmpfs",      TEST_ENABLED  };
    tests[t++] = (test_entry_t){ "/bin/test_ufs",        TEST_DISABLED }; /* pcxt only (UFS root) */
    tests[t++] = (test_entry_t){ "/bin/test_float",      TEST_ENABLED  };
/* TODO: qemu_rv32 signal delivery issues */
#if defined(__riscv)
    tests[t++] = (test_entry_t){ "/bin/test_signal_float", TEST_DISABLED };
#else
    tests[t++] = (test_entry_t){ "/bin/test_signal_float", TEST_ENABLED };
#endif
#if defined(__m68k__)
    tests[t++] = (test_entry_t){ "/bin/test_x68k",       TEST_ENABLED  };
    tests[t++] = (test_entry_t){ "/bin/test_h68k_dos",   TEST_ENABLED  };
#else
    tests[t++] = (test_entry_t){ "/bin/test_x68k",       TEST_DISABLED }; /* m68k only */
    tests[t++] = (test_entry_t){ "/bin/test_h68k_dos",   TEST_DISABLED }; /* m68k only */
#endif
/* TODO: qemu_rv32 CP/M Z80 instruction fetch fault */
#if defined(__riscv)
    tests[t++] = (test_entry_t){ "/bin/test_cpm",        TEST_DISABLED };
#else
    tests[t++] = (test_entry_t){ "/bin/test_cpm",        TEST_ENABLED  };
#endif
    tests[t++] = (test_entry_t){ "/bin/test_sos",        TEST_ENABLED  };
    tests[t++] = (test_entry_t){ "/bin/test_msdos",      TEST_DISABLED }; /* pcxt only */
    tests[t++] = (test_entry_t){ "/bin/test_zexdoc",     TEST_SLOW     };
    tests[t++] = (test_entry_t){ "/bin/test_zexall",     TEST_SLOW     };
    tests[t++] = (test_entry_t){ "/bin/test_musl",       TEST_ENABLED  };
    tests[t++] = (test_entry_t){ "/bin/test_trace",      TEST_ENABLED  };
#if defined(__m68k__)
    tests[t++] = (test_entry_t){ "/bin/test_pdb",        TEST_DISABLED };
#else
    tests[t++] = (test_entry_t){ "/bin/test_pdb",        TEST_SLOW     };
#endif
    tests[t].path = (void *)0;

    get_time(&g_start);
    read_filter();
    check_run_flaky();
    check_run_slow();

    print_ts();
    if (g_filter_len > 0) {
        print("=== PPAP on-target test suite (filter: ");
        print(g_filter);
        print(") ===\n");
    } else {
        print("=== PPAP on-target test suite ===\n");
    }

    int total = 0, passed = 0, failed = 0, skipped = 0;

    int i;
    for (i = 0; tests[i].path; i++) {
        const char *path  = tests[i].path;
        int         flags = tests[i].flags;

        if (!test_matches(path))
            continue;

        if (flags == TEST_DISABLED) {
            print_ts(); print("SKIP  "); print(path);
            print("  (disabled)\n");
            skipped++;
            continue;
        }
        if (flags == TEST_FLAKY && !g_run_flaky) {
            print_ts(); print("SKIP  "); print(path);
            print("  (flaky)\n");
            skipped++;
            continue;
        }
        if (flags == TEST_SLOW && !g_run_slow) {
            print_ts(); print("SKIP  "); print(path);
            print("  (slow)\n");
            skipped++;
            continue;
        }

        total++;
        struct rt_ts t_start;
        get_time(&t_start);
        print_ts(); print("RUN   "); print(path); print("\n");

        pid_t pid = vfork();
        if (pid == 0) {
            execve(path, (void *)0, (void *)0);
            _exit(127);   /* exec failed */
        }

        int status = 0;
        waitpid(pid, &status, 0);
        int code = (status >> 8) & 0xff;

        struct rt_ts t_end;
        get_time(&t_end);

        if (code != 0) {
            failed++;
            print_ts(); print("FAIL  "); print(path);
            print("  (exit "); print_int(code); print(", ");
            print_elapsed(&t_start, &t_end); print(")\n");
        } else {
            passed++;
            print_ts(); print("PASS  "); print(path);
            print("  ("); print_elapsed(&t_start, &t_end); print(")\n");
        }
    }

    print("\n");
    print_ts();
    print("=== Results: ");
    print_int(total);   print(" run, ");
    print_int(passed);  print(" passed, ");
    print_int(failed);  print(" failed, ");
    print_int(skipped); print(" skipped ===\n");

    if (failed == 0) {
        print_ts(); print("ALL TESTS PASSED\n");
    } else {
        print_ts(); print("SOME TESTS FAILED\n");
    }

    return failed;
}
#endif
