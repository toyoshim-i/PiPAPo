/*
 * main_qemu.c — Kernel entry point for QEMU mps2-an500 smoke test
 *
 * Replaces main.c for the ppap_qemu build target.  Exercises the same
 * kernel logic as main.c but adapts for QEMU's constraints:
 *
 *   - No clock_init_pll()      (QEMU has no PLL or XOSC to configure)
 *   - No uart_reinit_133mhz()  (clock never changes)
 *   - UART via CMSDK UART0     (uart_qemu.c, not uart.c)
 *   - Cooperative sched_yield() instead of SysTick preemption
 *     (QEMU mps2-an500 runs the SysTick counter but never asserts TICKINT;
 *      we use explicit sched_yield() calls to trigger PendSV instead)
 *
 * Phase 2 Step 10: syscall-level integration tests for the VFS stack.
 * Tests exercise sys_open, sys_read, sys_write, sys_close, sys_stat,
 * sys_getdents, sys_chdir, sys_getcwd, and VFS readlink through the
 * full fd → file → vfs_ops path.
 */

#include "drivers/uart.h"
#include "mm/page.h"
#include "mm/mpu.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "fd/fd.h"
#include "fd/file.h"
#include "vfs/vfs.h"
#include "fs/romfs.h"
#include "fs/devfs.h"
#include "fs/procfs.h"
#include "syscall/syscall.h"
#include "exec/exec.h"
#include "signal/signal.h"
#include "errno.h"
#include "smp.h"

/* Linker-provided romfs image location */
extern const uint8_t __romfs_start[];
extern const uint8_t __romfs_end[];

/* ── Integration test helpers ─────────────────────────────────────────────── */

static int test_pass;
static int test_fail;

static void test_report(const char *name, int ok)
{
    uart_puts("TEST: ");
    uart_puts(name);
    if (ok) {
        uart_puts(" ... PASS\n");
        test_pass++;
    } else {
        uart_puts(" ... FAIL\n");
        test_fail++;
    }
}

/* ── VFS integration tests ────────────────────────────────────────────────── */

static void vfs_integration_test(void)
{
    uart_puts("\n=== Phase 2 Step 10: VFS integration tests ===\n");
    test_pass = 0;
    test_fail = 0;

    /* 1. open + read /etc/hostname via syscall layer */
    {
        long fd = sys_open("/etc/hostname", O_RDONLY, 0);
        int ok = 0;
        if (fd >= 0) {
            char buf[32];
            long n = sys_read(fd, buf, sizeof(buf) - 1);
            if (n == 5) {
                buf[n] = '\0';
                /* "ppap\n" */
                ok = (buf[0]=='p' && buf[1]=='p' && buf[2]=='a' &&
                      buf[3]=='p' && buf[4]=='\n');
            }
            sys_close(fd);
        }
        test_report("open+read /etc/hostname", ok);
    }

    /* 2. open + read /etc/motd */
    {
        long fd = sys_open("/etc/motd", O_RDONLY, 0);
        int ok = 0;
        if (fd >= 0) {
            char buf[64];
            long n = sys_read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                /* starts with "Welcome" */
                ok = (buf[0]=='W' && buf[1]=='e' && buf[2]=='l');
            }
            sys_close(fd);
        }
        test_report("open+read /etc/motd", ok);
    }

    /* 3. open nonexistent file → -ENOENT */
    {
        long fd = sys_open("/nonexistent", O_RDONLY, 0);
        test_report("open /nonexistent → ENOENT", fd == -(long)ENOENT);
    }

    /* 4. open + write /dev/null */
    {
        long fd = sys_open("/dev/null", O_WRONLY, 0);
        int ok = 0;
        if (fd >= 0) {
            long n = sys_write(fd, "discarded", 9);
            ok = (n == 9);
            sys_close(fd);
        }
        test_report("open+write /dev/null", ok);
    }

    /* 5. open + read /dev/zero → all zero bytes */
    {
        long fd = sys_open("/dev/zero", O_RDONLY, 0);
        int ok = 0;
        if (fd >= 0) {
            char buf[4] = {0xFF, 0xFF, 0xFF, 0xFF};
            long n = sys_read(fd, buf, 4);
            if (n == 4)
                ok = (buf[0]==0 && buf[1]==0 && buf[2]==0 && buf[3]==0);
            sys_close(fd);
        }
        test_report("open+read /dev/zero", ok);
    }

    /* 6. open + read /proc/meminfo */
    {
        long fd = sys_open("/proc/meminfo", O_RDONLY, 0);
        int ok = 0;
        if (fd >= 0) {
            char buf[128];
            long n = sys_read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                /* should start with "MemTotal:" */
                ok = (buf[0]=='M' && buf[1]=='e' && buf[2]=='m' &&
                      buf[3]=='T');
            }
            sys_close(fd);
        }
        test_report("open+read /proc/meminfo", ok);
    }

    /* 7. stat /etc/hostname → regular file, size 5 */
    {
        struct stat st;
        long rc = sys_stat("/etc/hostname", &st);
        int ok = (rc == 0 && S_ISREG(st.st_mode) && st.st_size == 5);
        test_report("stat /etc/hostname", ok);
    }

    /* 8. stat /etc → directory */
    {
        struct stat st;
        long rc = sys_stat("/etc", &st);
        int ok = (rc == 0 && S_ISDIR(st.st_mode));
        test_report("stat /etc → DIR", ok);
    }

    /* 9. getdents / → should list "etc" and "bin" */
    {
        long fd = sys_open("/", O_RDONLY, 0);
        int ok = 0;
        if (fd >= 0) {
            struct dirent entries[8];
            long n = sys_getdents(fd, entries, 8);
            if (n >= 2) {
                int found_etc = 0, found_bin = 0;
                for (long i = 0; i < n; i++) {
                    if (__builtin_strcmp(entries[i].d_name, "etc") == 0)
                        found_etc = 1;
                    if (__builtin_strcmp(entries[i].d_name, "bin") == 0)
                        found_bin = 1;
                }
                ok = found_etc && found_bin;
            }
            sys_close(fd);
        }
        test_report("getdents /", ok);
    }

    /* 10. getdents /dev → should list device nodes */
    {
        long fd = sys_open("/dev", O_RDONLY, 0);
        int ok = 0;
        if (fd >= 0) {
            struct dirent entries[8];
            long n = sys_getdents(fd, entries, 8);
            if (n >= 4) {
                int found_null = 0, found_zero = 0;
                for (long i = 0; i < n; i++) {
                    if (__builtin_strcmp(entries[i].d_name, "null") == 0)
                        found_null = 1;
                    if (__builtin_strcmp(entries[i].d_name, "zero") == 0)
                        found_zero = 1;
                }
                ok = found_null && found_zero;
            }
            sys_close(fd);
        }
        test_report("getdents /dev", ok);
    }

    /* 11. chdir + getcwd */
    {
        long rc = sys_chdir("/etc");
        int ok = 0;
        if (rc == 0) {
            char buf[64];
            long n = sys_getcwd(buf, sizeof(buf));
            if (n > 0) {
                ok = (__builtin_strcmp(buf, "/etc") == 0);
            }
        }
        test_report("chdir+getcwd /etc", ok);
        /* Restore cwd to / */
        sys_chdir("/");
    }

    /* 12. stat /bin/hello — user-space ELF binary in romfs (Phase 3 Step 1) */
    {
        struct stat st;
        int ok = (sys_stat("/bin/hello", &st) == 0
                  && (st.st_mode & 0170000) == 0100000  /* S_IFREG */
                  && st.st_size > 0);
        test_report("stat /bin/hello → regular file", ok);
    }

    /* Summary */
    uart_puts("=== Results: ");
    /* Print pass count */
    char digit[4];
    int idx = 0;
    int v = test_pass;
    if (v >= 10) digit[idx++] = '0' + (v / 10);
    digit[idx++] = '0' + (v % 10);
    digit[idx] = '\0';
    uart_puts(digit);
    uart_puts(" passed, ");
    idx = 0;
    v = test_fail;
    if (v >= 10) digit[idx++] = '0' + (v / 10);
    digit[idx++] = '0' + (v % 10);
    digit[idx] = '\0';
    uart_puts(digit);
    uart_puts(" failed ===\n\n");
}

/* ── Pipe integration tests ──────────────────────────────────────────────── */

static void pipe_integration_test(void)
{
    uart_puts("\n=== Phase 3 Step 7: Pipe integration tests ===\n");
    test_pass = 0;
    test_fail = 0;

    /* 1. pipe + write + read */
    {
        int fds[2];
        long rc = sys_pipe(fds);
        int ok = 0;
        if (rc == 0) {
            long nw = sys_write(fds[1], "hello", 5);
            if (nw == 5) {
                char buf[8] = {0};
                long nr = sys_read(fds[0], buf, sizeof(buf));
                if (nr == 5)
                    ok = (buf[0]=='h' && buf[1]=='e' && buf[2]=='l'
                       && buf[3]=='l' && buf[4]=='o');
            }
            sys_close(fds[0]);
            sys_close(fds[1]);
        }
        test_report("pipe write+read", ok);
    }

    /* 2. pipe EOF: close write end, read → returns 0 */
    {
        int fds[2];
        long rc = sys_pipe(fds);
        int ok = 0;
        if (rc == 0) {
            sys_close(fds[1]);   /* close write end */
            char buf[4];
            long nr = sys_read(fds[0], buf, sizeof(buf));
            ok = (nr == 0);      /* EOF */
            sys_close(fds[0]);
        }
        test_report("pipe EOF on close", ok);
    }

    /* 3. pipe EPIPE: close read end, write → returns -EPIPE */
    {
        int fds[2];
        long rc = sys_pipe(fds);
        int ok = 0;
        if (rc == 0) {
            sys_close(fds[0]);   /* close read end */
            long nw = sys_write(fds[1], "x", 1);
            ok = (nw == -(long)EPIPE);
            sys_close(fds[1]);
        }
        test_report("pipe EPIPE on close", ok);
    }

    /* 4. pipe partial fill: fill buffer, verify partial write count */
    {
        int fds[2];
        long rc = sys_pipe(fds);
        int ok = 0;
        if (rc == 0) {
            /* PIPE_BUF_SIZE=512, usable=511 (1-byte gap).
             * Write 256 bytes twice: first succeeds fully, second is partial. */
            char wbuf[256];
            for (int i = 0; i < 256; i++) wbuf[i] = (char)i;

            long n1 = sys_write(fds[1], wbuf, 256);
            long n2 = sys_write(fds[1], wbuf, 256);
            /* n1=256, n2=255 (only 255 bytes of space left) */
            ok = (n1 == 256 && n2 == 255);

            /* Drain and verify first byte */
            char rbuf[512];
            long nr = sys_read(fds[0], rbuf, sizeof(rbuf));
            if (nr == 511)
                ok = ok && (rbuf[0] == 0);

            sys_close(fds[0]);
            sys_close(fds[1]);
        }
        test_report("pipe partial fill", ok);
    }

    /* 5. pipe close cleans up: close both ends, pipe freed */
    {
        int fds[2];
        long rc = sys_pipe(fds);
        int ok = 0;
        if (rc == 0) {
            sys_close(fds[0]);
            sys_close(fds[1]);
            /* Allocating another pipe should succeed (slot reused) */
            int fds2[2];
            long rc2 = sys_pipe(fds2);
            ok = (rc2 == 0);
            if (rc2 == 0) {
                sys_close(fds2[0]);
                sys_close(fds2[1]);
            }
        }
        test_report("pipe close cleanup", ok);
    }

    /* Summary */
    uart_puts("=== Pipe results: ");
    char digit[4];
    int idx = 0;
    int v = test_pass;
    if (v >= 10) digit[idx++] = '0' + (v / 10);
    digit[idx++] = '0' + (v % 10);
    digit[idx] = '\0';
    uart_puts(digit);
    uart_puts(" passed, ");
    idx = 0;
    v = test_fail;
    if (v >= 10) digit[idx++] = '0' + (v / 10);
    digit[idx++] = '0' + (v % 10);
    digit[idx] = '\0';
    uart_puts(digit);
    uart_puts(" failed ===\n\n");
}

/* ── dup/dup2 integration tests ──────────────────────────────────────────── */

static void dup_integration_test(void)
{
    uart_puts("\n=== Phase 3 Step 8: dup/dup2 integration tests ===\n");
    test_pass = 0;
    test_fail = 0;

    /* 1. dup basic: pipe, dup read-end, read from dup'd fd */
    {
        int fds[2];
        long rc = sys_pipe(fds);
        int ok = 0;
        if (rc == 0) {
            long dup_fd = sys_dup(fds[0]);
            if (dup_fd >= 0) {
                sys_write(fds[1], "dup!", 4);
                char buf[8] = {0};
                long nr = sys_read(dup_fd, buf, sizeof(buf));
                ok = (nr == 4 && buf[0]=='d' && buf[1]=='u'
                   && buf[2]=='p' && buf[3]=='!');
                sys_close(dup_fd);
            }
            sys_close(fds[0]);
            sys_close(fds[1]);
        }
        test_report("dup basic", ok);
    }

    /* 2. dup2 redirect: pipe + dup2(write_end, 4) → write via fd 4 */
    {
        int fds[2];
        long rc = sys_pipe(fds);
        int ok = 0;
        if (rc == 0) {
            long rc2 = sys_dup2(fds[1], 4);
            if (rc2 == 4) {
                sys_write(4, "hi", 2);
                char buf[4] = {0};
                long nr = sys_read(fds[0], buf, sizeof(buf));
                ok = (nr == 2 && buf[0]=='h' && buf[1]=='i');
            }
            sys_close(4);
            sys_close(fds[0]);
            sys_close(fds[1]);
        }
        test_report("dup2 redirect", ok);
    }

    /* 3. dup2 same fd: dup2(fd, fd) → returns fd (no-op) */
    {
        int fds[2];
        long rc = sys_pipe(fds);
        int ok = 0;
        if (rc == 0) {
            long rc2 = sys_dup2(fds[0], fds[0]);
            ok = (rc2 == fds[0]);
            sys_close(fds[0]);
            sys_close(fds[1]);
        }
        test_report("dup2 same fd", ok);
    }

    /* 4. dup2 closes target: dup2 over an open pipe fd */
    {
        int fds_a[2], fds_b[2];
        long rca = sys_pipe(fds_a);
        long rcb = sys_pipe(fds_b);
        int ok = 0;
        if (rca == 0 && rcb == 0) {
            /* dup2(fds_a[0], fds_b[0]) — closes fds_b[0] (read end of pipe B) */
            sys_dup2(fds_a[0], fds_b[0]);
            /* Writing to pipe B write-end should now get EPIPE since
             * its read-end was closed by dup2 */
            long nw = sys_write(fds_b[1], "x", 1);
            ok = (nw == -(long)EPIPE);
            sys_close(fds_a[0]);
            sys_close(fds_a[1]);
            sys_close(fds_b[0]);  /* now points to fds_a's read file */
            sys_close(fds_b[1]);
        }
        test_report("dup2 close target", ok);
    }

    /* 5. dup invalid fd → -EBADF */
    {
        long rc = sys_dup(-1);
        test_report("dup invalid fd", rc == -(long)EBADF);
    }

    /* Summary */
    uart_puts("=== dup/dup2 results: ");
    char digit[4];
    int idx = 0;
    int v = test_pass;
    if (v >= 10) digit[idx++] = '0' + (v / 10);
    digit[idx++] = '0' + (v % 10);
    digit[idx] = '\0';
    uart_puts(digit);
    uart_puts(" passed, ");
    idx = 0;
    v = test_fail;
    if (v >= 10) digit[idx++] = '0' + (v / 10);
    digit[idx++] = '0' + (v % 10);
    digit[idx] = '\0';
    uart_puts(digit);
    uart_puts(" failed ===\n\n");
}

/* ── brk integration tests ───────────────────────────────────────────────── */

static void brk_integration_test(void)
{
    uart_puts("\n=== Phase 3 Step 9: brk integration tests ===\n");
    test_pass = 0;
    test_fail = 0;

    /* Set up a fake data page for proc_table[0] so sys_brk works.
     * In real use, do_execve sets brk_base/brk_current. */
    void *fake_page = page_alloc();
    current->user_pages[0] = fake_page;
    uint32_t base = (uint32_t)(uintptr_t)fake_page + 256;  /* pretend 256B used */
    current->brk_base    = base;
    current->brk_current = base;

    /* 1. brk query: sys_brk(0) → returns current break */
    {
        long rc = sys_brk(0);
        test_report("brk query", rc == (long)base);
    }

    /* 2. brk expand within same page */
    {
        long rc = sys_brk((long)(base + 64));
        test_report("brk expand +64", rc == (long)(base + 64));
    }

    /* 3. brk below base → -ENOMEM */
    {
        long rc = sys_brk((long)(base - 1));
        test_report("brk below base", rc == -(long)ENOMEM);
    }

    /* Clean up */
    current->brk_base = 0;
    current->brk_current = 0;
    page_free(fake_page);
    current->user_pages[0] = NULL;

    /* Summary */
    uart_puts("=== brk results: ");
    char digit[4];
    int idx = 0;
    int v = test_pass;
    if (v >= 10) digit[idx++] = '0' + (v / 10);
    digit[idx++] = '0' + (v % 10);
    digit[idx] = '\0';
    uart_puts(digit);
    uart_puts(" passed, ");
    idx = 0;
    v = test_fail;
    if (v >= 10) digit[idx++] = '0' + (v / 10);
    digit[idx++] = '0' + (v % 10);
    digit[idx] = '\0';
    uart_puts(digit);
    uart_puts(" failed ===\n\n");
}

/* ── Phase 3 Step 10: signal integration tests ───────────────────────────── */

/* Dummy handler address for testing (never actually called from kernel) */
static void dummy_sig_handler(int sig) { (void)sig; }

static void signal_integration_test(void)
{
    uart_puts("\n=== Phase 3 Step 10: signal integration tests ===\n");
    test_pass = 0;
    test_fail = 0;

    /* 1. sigaction installs handler */
    {
        long rc = sys_sigaction(10 /* SIGUSR1 */,
                                (long)(uintptr_t)dummy_sig_handler, 0);
        test_report("sigaction install",
                    rc == 0 &&
                    current->sig_handlers[10] ==
                        (sighandler_t)(uintptr_t)dummy_sig_handler);
    }

    /* 2. SIGKILL cannot be caught */
    {
        long rc = sys_sigaction(9 /* SIGKILL */,
                                (long)(uintptr_t)dummy_sig_handler, 0);
        test_report("SIGKILL reject", rc == -(long)EINVAL);
    }

    /* 3. sys_kill sets pending bit */
    {
        current->sig_pending = 0;
        long rc = sys_kill((long)current->pid, 10 /* SIGUSR1 */);
        test_report("kill sets pending",
                    rc == 0 && (current->sig_pending & (1u << 10)));
    }

    /* 4. sigaction returns old handler */
    {
        sighandler_t old = 0;
        long rc = sys_sigaction(10 /* SIGUSR1 */,
                                0 /* SIG_DFL */,
                                (long)(uintptr_t)&old);
        test_report("sigaction query old",
                    rc == 0 &&
                    old == (sighandler_t)(uintptr_t)dummy_sig_handler);
    }

    /* Clean up */
    current->sig_pending = 0;
    current->sig_blocked = 0;
    for (int i = 0; i < 32; i++)
        current->sig_handlers[i] = (sighandler_t)0;

    /* Summary */
    uart_puts("=== signal results: ");
    char digit[4];
    int idx = 0;
    int v = test_pass;
    if (v >= 10) digit[idx++] = '0' + (v / 10);
    digit[idx++] = '0' + (v % 10);
    digit[idx] = '\0';
    uart_puts(digit);
    uart_puts(" passed, ");
    idx = 0;
    v = test_fail;
    if (v >= 10) digit[idx++] = '0' + (v / 10);
    digit[idx++] = '0' + (v % 10);
    digit[idx] = '\0';
    uart_puts(digit);
    uart_puts(" failed ===\n\n");
}

/* ── Context-switch partner (prints "1" in a loop) ───────────────────────── */

static void thread_loop(void)
{
    for (;;) {
        uart_puts("1\n");
        sched_yield();
    }
}

/* ── Kernel entry point ──────────────────────────────────────────────────── */

void kmain(void)
{
    uart_init_console();
    uart_puts("PicoPiAndPortable booting (QEMU mps2-an500)...\n");
    uart_puts("UART: CMSDK UART0 @ 0x40004000\n");
    uart_puts("Clock: emulated (no PLL — skipping clock_init_pll)\n");

    mm_init();

    /* Phase 1 Step 3: process table init */
    proc_init();

    /* Phase 2 Steps 1-3: VFS layer + file pool for sys_open */
    vfs_init();
    file_pool_init();

    /* Phase 2 Steps 7-9: mount romfs, devfs, procfs */
    if (vfs_mount("/", &romfs_ops, MNT_RDONLY, __romfs_start) == 0)
        uart_puts("VFS: romfs mounted at /\n");
    else
        uart_puts("VFS: romfs mount FAILED\n");

    if (vfs_mount("/dev", &devfs_ops, 0, NULL) == 0)
        uart_puts("VFS: devfs mounted at /dev\n");
    else
        uart_puts("VFS: devfs mount FAILED\n");

    if (vfs_mount("/proc", &procfs_ops, MNT_RDONLY, NULL) == 0)
        uart_puts("VFS: procfs mounted at /proc\n");
    else
        uart_puts("VFS: procfs mount FAILED\n");

    /* Phase 1 Step 10: wire fd 0/1/2 to the UART tty driver */
    fd_stdio_init(&proc_table[0]);

    /* Phase 2 Step 10: syscall-level VFS integration tests */
    vfs_integration_test();

    /* Phase 3 Step 7: pipe integration tests */
    pipe_integration_test();

    /* Phase 3 Step 8: dup/dup2 integration tests */
    dup_integration_test();

    /* Phase 3 Step 9: brk integration tests */
    brk_integration_test();

    /* Phase 3 Step 10: signal integration tests */
    signal_integration_test();

    /* ------------------------------------------------------------------
     * Phase 3 Step 5: exec /bin/test_vfork as the init process (pid 1)
     * Tests vfork + execve + waitpid integration.
     * ------------------------------------------------------------------ */
    proc_table[0].stack_page = page_alloc();

    pcb_t *init = proc_alloc();
    int exec_err = do_execve(init, "/bin/test_vfork");
    if (exec_err == 0) {
        init->state = PROC_RUNNABLE;
        uart_puts("EXEC: /bin/test_vfork loaded, pid=");
        uart_print_dec(init->pid);
        uart_puts("\n");
    } else {
        uart_puts("EXEC: /bin/test_vfork FAILED (err=");
        uart_print_dec((uint32_t)(-(int)exec_err));
        uart_puts(")\n");
    }

    /* Also create a plain context-switch partner thread */
    pcb_t *p2 = proc_alloc();
    p2->stack_page = page_alloc();
    proc_setup_stack(p2, thread_loop);
    p2->state = PROC_RUNNABLE;

    /* Phase 1 Step 11: configure MPU (no-op on QEMU — MPU_TYPE reads 0) */
    mpu_init();

    /* Phase 1 Step 12: launch Core 1 (no-op on QEMU — SIO not mapped) */
    core1_launch(core1_io_worker);

    uart_puts("SCHED: starting scheduler (QEMU)\n");
    sched_start();

    /* Thread 0 continues here — print "0\n" and yield each iteration */
    for (;;) {
        uart_puts("0\n");
        sched_yield();
    }
}
