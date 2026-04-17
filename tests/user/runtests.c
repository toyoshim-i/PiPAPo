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
 *     Disabled at runtime if clock_gettime is unavailable on the target.
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

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* Decimal print using only subtract+compare so no libgcc divide is required
 * (works on ARM Cortex-M0+ kernels and the ia16 tiny model alike). */
static void print_int(int v)
{
    unsigned int x;
    char buf[12];   /* enough for 32-bit unsigned */
    int n = 0;

    if (v < 0) {
        write(1, "-", 1);
        x = (unsigned int)(-v);
    } else {
        x = (unsigned int)v;
    }

    do {
        unsigned int q = 0;
        while (x >= 10u) { x -= 10u; q++; }
        buf[n++] = (char)('0' + x);
        x = q;
    } while (x > 0u);

    while (n > 0) {
        char c = buf[--n];
        write(1, &c, 1);
    }
}

/* Print two-digit decimal 00-99 without division */
static void print_dd(int v)
{
    int tens = 0;
    while (v >= 10) { v -= 10; tens++; }
    char buf[2];
    buf[0] = '0' + tens;
    buf[1] = '0' + v;
    write(1, buf, 2);
}

/* ── Timestamps (runtime-disabled if clock_gettime is unavailable) ──────── */

struct rt_ts { long tv_sec; long tv_nsec; };

static struct rt_ts g_start;
static int          g_ts_ok;     /* 1 if clock_gettime works on this target */

static int get_time(struct rt_ts *ts)
{
    return clock_gettime(1 /* CLOCK_MONOTONIC */, ts);
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
    if (!g_ts_ok) return;
    struct rt_ts now;
    if (get_time(&now) < 0) return;
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
    if (!g_ts_ok) return;
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

/* ── Flaky / slow opt-ins ────────────────────────────────────────────────── */

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

/* ── Stdout redirect ─────────────────────────────────────────────────────── */

/* On pcxt the default fd 1 is the VGA console, invisible to QEMU's serial
 * capture.  Redirect to /dev/ttyS0 if it can be opened.  Other targets
 * already point fd 1 at the serial console; the redirect is a no-op there
 * (re-binding fd 1 to the same underlying device). */
static void redirect_stdout_to_serial(void)
{
    int sfd = open("/dev/ttyS0", 1 /* O_WRONLY */, 0);
    if (sfd < 0) return;
    close(1);
    dup(sfd);
    close(sfd);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    /* Initialize the test list at runtime — PIC binaries don't support
     * static pointer arrays because execve only relocates GOT entries,
     * not initialized data pointers.  Runtime assignment uses GOT-resolved
     * addresses which are correctly relocated. */
    test_entry_t tests[34];
    int t = 0;

    /* Tests not built for pcxt (ia16 tiny model) are marked DISABLED for
     * __ia16__.  Other per-target ifdefs disable known failures awaiting
     * a real fix. */

    tests[t++] = (test_entry_t){ "/bin/test_exec",
#if defined(__riscv)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_elf",
#if defined(__riscv) || defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_vfork", TEST_ENABLED };
    tests[t++] = (test_entry_t){ "/bin/test_fault",
#if defined(__riscv) || defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_pipe",
#if defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_brk",
#if defined(__riscv) || defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_fd",
#if defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_signal",
#if defined(__riscv) || defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_poll",
#if defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_sleep_intr",
#if defined(__riscv) || defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_orphan",
#if defined(__riscv) || defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_id",
#if defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_fs",
#if defined(__riscv)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_rw", TEST_ENABLED };
    tests[t++] = (test_entry_t){ "/bin/test_time",
#if defined(__riscv) || defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_iov",
#if defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_stat",
#if defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_tmpfs",
#if defined(__riscv) || defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    /* test_ufs runs only where a UFS root is mounted (pcxt today). */
    tests[t++] = (test_entry_t){ "/bin/test_ufs",
#if defined(__ia16__)
        TEST_ENABLED
#else
        TEST_DISABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_float",
#if defined(__m68k__) || defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_signal_float",
#if defined(__m68k__) || defined(__riscv) || defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_x68k",
#if defined(__m68k__)
        TEST_ENABLED
#else
        TEST_DISABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_h68k_dos",
#if defined(__m68k__)
        TEST_ENABLED
#else
        TEST_DISABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_cpm",
#if defined(__riscv) || defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_sos",
#if defined(__riscv) || defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    /* test_msdos: pcxt-only (floppy/MSDOS subsystem); under investigation. */
    tests[t++] = (test_entry_t){ "/bin/test_msdos", TEST_DISABLED };
    tests[t++] = (test_entry_t){ "/bin/test_zexdoc",
#if defined(__ia16__)
        TEST_DISABLED
#else
        TEST_SLOW
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_zexall",
#if defined(__ia16__)
        TEST_DISABLED
#else
        TEST_SLOW
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_musl",
#if defined(__ia16__)
        TEST_DISABLED
#else
        TEST_ENABLED
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_trace",
#if defined(__ia16__)
        TEST_DISABLED
#else
        TEST_FLAKY
#endif
    };
    tests[t++] = (test_entry_t){ "/bin/test_pdb",
#if defined(__m68k__) || defined(__ia16__)
        TEST_DISABLED
#else
        TEST_SLOW
#endif
    };

    tests[t].path = (void *)0;

    redirect_stdout_to_serial();

    g_ts_ok = (get_time(&g_start) == 0);
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
            print("  (exit "); print_int(code);
            if (g_ts_ok) { print(", "); print_elapsed(&t_start, &t_end); }
            print(")\n");
        } else {
            passed++;
            print_ts(); print("PASS  "); print(path);
            if (g_ts_ok) { print("  ("); print_elapsed(&t_start, &t_end); print(")"); }
            print("\n");
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

    poweroff();
    return failed;
}
