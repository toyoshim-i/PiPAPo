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
#include "blkdev/blkdev.h"
#include "blkdev/ramblk.h"
#include "blkdev/loopback.h"
#include "fs/vfat.h"
#include "errno.h"
#include "smp.h"

/* Linker-provided romfs image location */
extern const uint8_t __romfs_start[];
extern const uint8_t __romfs_end[];

/* Linker-provided FAT32 image location (populated in Step 8) */
extern const uint8_t __fatimg_start[];
extern const uint8_t __fatimg_end[];

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

/* ── Phase 4 Step 1: blkdev integration tests ────────────────────────────── */

/* Small test buffer: 8 sectors (4 KB) allocated from page pool */
static void blkdev_integration_test(void)
{
    uart_puts("\n=== Phase 4 Step 1: blkdev integration tests ===\n");
    test_pass = 0;
    test_fail = 0;

    /* 1. blkdev_init + register: ramblk already called from kmain */
    {
        blkdev_t *bd = blkdev_find("mmcblk0");
        test_report("blkdev register", bd != (void *)0);
    }

    /* 2. blkdev_find by name */
    {
        blkdev_t *bd = blkdev_find("mmcblk0");
        int ok = (bd != (void *)0 && bd->sector_count > 0);
        test_report("blkdev find", ok);
    }

    /* 3. blkdev_find nonexistent → NULL */
    {
        blkdev_t *bd = blkdev_find("nodev");
        test_report("blkdev find nonexistent", bd == (void *)0);
    }

    /* 4. read/write round-trip: write a pattern, read it back */
    {
        blkdev_t *bd = blkdev_find("mmcblk0");
        int ok = 0;
        if (bd) {
            uint8_t wbuf[BLKDEV_SECTOR_SIZE];
            uint8_t rbuf[BLKDEV_SECTOR_SIZE];

            /* Fill write buffer with a pattern */
            for (uint32_t i = 0; i < BLKDEV_SECTOR_SIZE; i++)
                wbuf[i] = (uint8_t)(i & 0xFF);

            /* Write sector 1, then read it back */
            int wrc = bd->write(bd, wbuf, 1, 1);
            int rrc = bd->read(bd, rbuf, 1, 1);

            if (wrc == 0 && rrc == 0) {
                ok = 1;
                for (uint32_t i = 0; i < BLKDEV_SECTOR_SIZE; i++) {
                    if (rbuf[i] != wbuf[i]) { ok = 0; break; }
                }
            }
        }
        test_report("blkdev read/write round-trip", ok);
    }

    /* 5. read sector 0: verify it reads successfully (BPB or test pattern) */
    {
        blkdev_t *bd = blkdev_find("mmcblk0");
        int ok = 0;
        if (bd) {
            uint8_t rbuf[BLKDEV_SECTOR_SIZE];
            int rrc = bd->read(bd, rbuf, 0, 1);
            /* Sector 0 is either a FAT32 BPB (0xEB...) or test pattern (0xAA) */
            ok = (rrc == 0 && (rbuf[0] == 0xEB || rbuf[0] == 0xAA));
        }
        test_report("blkdev read sector 0", ok);
    }

    /* 6. /dev/mmcblk0 accessible via devfs */
    {
        long fd = sys_open("/dev/mmcblk0", O_RDONLY, 0);
        test_report("/dev/mmcblk0 open", fd >= 0);
        if (fd >= 0)
            sys_close(fd);
    }

    /* Summary */
    uart_puts("=== blkdev results: ");
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

/* ── Phase 4 Step 9: VFAT integration tests ──────────────────────────────── */

static void vfat_integration_test(void)
{
    uart_puts("\n=== Phase 4 Step 9: VFAT integration tests ===\n");
    test_pass = 0;
    test_fail = 0;

    /* 1. Read /mnt/sd/hello.txt */
    {
        long fd = sys_open("/mnt/sd/hello.txt", O_RDONLY, 0);
        int ok = 0;
        if (fd >= 0) {
            char buf[32];
            long n = sys_read(fd, buf, sizeof(buf) - 1);
            if (n == 19) {
                buf[n] = '\0';
                /* "Hello from FAT32!\n\0" (18 chars + NUL = 19 bytes) */
                ok = (buf[0] == 'H' && buf[5] == ' '
                   && buf[11] == 'F' && buf[16] == '!' && buf[17] == '\n');
            }
            sys_close(fd);
        }
        test_report("read /mnt/sd/hello.txt", ok);
    }

    /* 2. Read /mnt/sd/data.bin (256 bytes: 0x00..0xFF) */
    {
        long fd = sys_open("/mnt/sd/data.bin", O_RDONLY, 0);
        int ok = 0;
        if (fd >= 0) {
            char buf[256];
            long n = sys_read(fd, buf, 256);
            if (n == 256) {
                ok = 1;
                for (int i = 0; i < 256; i++) {
                    if ((uint8_t)buf[i] != (uint8_t)i) { ok = 0; break; }
                }
            }
            sys_close(fd);
        }
        test_report("read /mnt/sd/data.bin", ok);
    }

    /* 3. stat /mnt/sd/hello.txt — regular file, 19 bytes */
    {
        struct stat st;
        long rc = sys_stat("/mnt/sd/hello.txt", &st);
        int ok = (rc == 0 && S_ISREG(st.st_mode) && st.st_size == 19);
        test_report("stat /mnt/sd/hello.txt", ok);
    }

    /* 4. stat /mnt/sd/subdir — directory */
    {
        struct stat st;
        long rc = sys_stat("/mnt/sd/subdir", &st);
        int ok = (rc == 0 && S_ISDIR(st.st_mode));
        test_report("stat /mnt/sd/subdir", ok);
    }

    /* 5. getdents /mnt/sd → list hello.txt, data.bin, subdir */
    {
        long fd = sys_open("/mnt/sd", O_RDONLY, 0);
        int ok = 0;
        if (fd >= 0) {
            struct dirent entries[8];
            long n = sys_getdents(fd, entries, 8);
            if (n >= 3) {
                int found_hello = 0, found_data = 0, found_subdir = 0;
                for (long i = 0; i < n; i++) {
                    if (__builtin_strcmp(entries[i].d_name, "hello.txt") == 0)
                        found_hello = 1;
                    if (__builtin_strcmp(entries[i].d_name, "data.bin") == 0)
                        found_data = 1;
                    if (__builtin_strcmp(entries[i].d_name, "subdir") == 0)
                        found_subdir = 1;
                }
                ok = found_hello && found_data && found_subdir;
            }
            sys_close(fd);
        }
        test_report("getdents /mnt/sd", ok);
    }

    /* 6. Create + write + read a new file */
    {
        long fd = sys_open("/mnt/sd/newfile.txt", O_WRONLY | O_CREAT, 0644);
        int ok = 0;
        if (fd >= 0) {
            long nw = sys_write(fd, "test data", 9);
            sys_close(fd);
            if (nw == 9) {
                /* Read it back */
                fd = sys_open("/mnt/sd/newfile.txt", O_RDONLY, 0);
                if (fd >= 0) {
                    char buf[16] = {0};
                    long nr = sys_read(fd, buf, sizeof(buf));
                    ok = (nr == 9 && buf[0] == 't' && buf[4] == ' '
                       && buf[8] == 'a');
                    sys_close(fd);
                }
            }
        }
        test_report("create+write+read newfile", ok);
    }

    /* 7. mkdir /mnt/sd/testdir */
    {
        long rc = sys_mkdir("/mnt/sd/testdir", 0755);
        int ok = 0;
        if (rc == 0) {
            struct stat st;
            ok = (sys_stat("/mnt/sd/testdir", &st) == 0 && S_ISDIR(st.st_mode));
        }
        test_report("mkdir /mnt/sd/testdir", ok);
    }

    /* 8. unlink /mnt/sd/newfile.txt */
    {
        long rc = sys_unlink("/mnt/sd/newfile.txt");
        int ok = 0;
        if (rc == 0) {
            /* Should no longer exist */
            ok = (sys_stat("/mnt/sd/newfile.txt", &(struct stat){0}) == -(long)ENOENT);
        }
        test_report("unlink /mnt/sd/newfile.txt", ok);
    }

    /* 9. lseek + partial read */
    {
        long fd = sys_open("/mnt/sd/data.bin", O_RDONLY, 0);
        int ok = 0;
        if (fd >= 0) {
            sys_lseek(fd, 128, SEEK_SET);
            char buf[4];
            long n = sys_read(fd, buf, 4);
            ok = (n == 4 && (uint8_t)buf[0] == 128 && (uint8_t)buf[1] == 129
               && (uint8_t)buf[2] == 130 && (uint8_t)buf[3] == 131);
            sys_close(fd);
        }
        test_report("lseek+read /mnt/sd/data.bin", ok);
    }

    /* 10. open nonexistent on vfat → -ENOENT */
    {
        long fd = sys_open("/mnt/sd/nofile", O_RDONLY, 0);
        test_report("open nonexistent on vfat", fd == -(long)ENOENT);
    }

    /* Summary */
    uart_puts("=== VFAT results: ");
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

/* ── Phase 5 Step 1: Loopback integration tests ─────────────────────────── */

static void loopback_integration_test(void)
{
    uart_puts("\n=== Phase 5 Step 1: loopback integration tests ===\n");
    test_pass = 0;
    test_fail = 0;

    /* Check if VFAT is mounted (need a real filesystem to test loopback) */
    {
        vnode_t *vn = (void *)0;
        int rc = vfs_lookup("/mnt/sd", &vn);
        if (rc < 0) {
            uart_puts("SKIP: /mnt/sd not mounted, cannot test loopback\n");
            return;
        }
        vnode_put(vn);
    }

    /* 1. Verify pre-populated testloop.bin exists (from mkfatimg) */
    {
        struct stat st;
        long rc = sys_stat("/mnt/sd/testloop.bin", &st);
        int ok = (rc == 0 && st.st_size == 2048);
        test_report("stat /mnt/sd/testloop.bin (2048 B)", ok);
        if (!ok) return;  /* can't continue without the test file */
    }

    /* 2. Set up loopback device from the test file */
    {
        int idx = loopback_setup("/mnt/sd/testloop.bin");
        test_report("loopback_setup → loop0", idx == 0);
        if (idx < 0) return;  /* can't continue */
    }

    /* 3. Verify blkdev registered */
    {
        blkdev_t *bd = blkdev_find("loop0");
        test_report("blkdev_find(loop0)", bd != (void *)0);
    }

    /* 4. Verify sector count */
    {
        blkdev_t *bd = blkdev_find("loop0");
        int ok = (bd && bd->sector_count == 4);
        test_report("loop0 sector_count == 4", ok);
    }

    /* 5. Read sector 0 and verify pattern */
    {
        blkdev_t *bd = blkdev_find("loop0");
        int ok = 0;
        if (bd) {
            uint8_t buf[BLKDEV_SECTOR_SIZE];
            int rc = bd->read(bd, buf, 0, 1);
            if (rc == 0) {
                ok = 1;
                for (int i = 0; i < (int)BLKDEV_SECTOR_SIZE; i++) {
                    if (buf[i] != 0x00) { ok = 0; break; }
                }
            }
        }
        test_report("read sector 0 (pattern 0x00)", ok);
    }

    /* 6. Read sector 3 and verify pattern */
    {
        blkdev_t *bd = blkdev_find("loop0");
        int ok = 0;
        if (bd) {
            uint8_t buf[BLKDEV_SECTOR_SIZE];
            int rc = bd->read(bd, buf, 3, 1);
            if (rc == 0) {
                ok = 1;
                for (int i = 0; i < (int)BLKDEV_SECTOR_SIZE; i++) {
                    if (buf[i] != 0x03) { ok = 0; break; }
                }
            }
        }
        test_report("read sector 3 (pattern 0x03)", ok);
    }

    /* 7. Read out-of-range sector → EIO */
    {
        blkdev_t *bd = blkdev_find("loop0");
        int ok = 0;
        if (bd) {
            uint8_t buf[BLKDEV_SECTOR_SIZE];
            int rc = bd->read(bd, buf, 4, 1);
            ok = (rc == -EIO);
        }
        test_report("read sector 4 (out of range) → EIO", ok);
    }

    /* 8. Write sector 0 + read back */
    {
        blkdev_t *bd = blkdev_find("loop0");
        int ok = 0;
        if (bd) {
            uint8_t wbuf[BLKDEV_SECTOR_SIZE];
            __builtin_memset(wbuf, 0xAB, BLKDEV_SECTOR_SIZE);
            int rc = bd->write(bd, wbuf, 0, 1);
            if (rc == 0) {
                uint8_t rbuf[BLKDEV_SECTOR_SIZE];
                rc = bd->read(bd, rbuf, 0, 1);
                if (rc == 0) {
                    ok = 1;
                    for (int i = 0; i < (int)BLKDEV_SECTOR_SIZE; i++) {
                        if (rbuf[i] != 0xAB) { ok = 0; break; }
                    }
                }
            }
        }
        test_report("write sector 0 + read back (0xAB)", ok);
    }

    /* 9. loopback_is_active check */
    {
        test_report("loopback_is_active(0)", loopback_is_active(0) == 1);
        test_report("loopback_is_active(1)", loopback_is_active(1) == 0);
    }

    /* 10. Teardown loop0 */
    {
        int rc = loopback_teardown(0);
        test_report("loopback_teardown(0)", rc == 0);
        test_report("is_active(0) after teardown", loopback_is_active(0) == 0);
    }

    /* 11. Teardown inactive → EINVAL */
    {
        int rc = loopback_teardown(0);
        test_report("teardown inactive → EINVAL", rc == -EINVAL);
    }

    /* Summary */
    uart_puts("Phase 5 Step 1 loopback: ");
    uart_print_dec((uint32_t)test_pass);
    uart_puts(" passed, ");
    uart_print_dec((uint32_t)test_fail);
    uart_puts(" failed\n");

    /* testloop.bin is ROM-resident (from mkfatimg), no cleanup needed */
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

    /* Phase 4 Step 1: block device layer + RAM-backed device.
     * If a FAT32 image is embedded (.fatimg section), use it.
     * Otherwise fall back to a small test-pattern image. */
    blkdev_init();
    loopback_init();
    {
        uint32_t fatimg_size = (uint32_t)(__fatimg_end - __fatimg_start);
        if (fatimg_size >= BLKDEV_SECTOR_SIZE) {
            /* ROM-embedded FAT32 image */
            int rc = ramblk_init(__fatimg_start, fatimg_size);
            if (rc >= 0) {
                uart_puts("BLKDEV: ramblk mmcblk0 (FAT32 image, ");
                uart_print_dec(fatimg_size / 1024);
                uart_puts(" KB)\n");
            } else {
                uart_puts("BLKDEV: ramblk init FAILED\n");
            }
        } else {
            /* No FAT32 image — use test pattern (4 KB = 8 sectors) */
            uint8_t *test_img = (uint8_t *)page_alloc();
            if (test_img) {
                __builtin_memset(test_img, 0, PAGE_SIZE);
                __builtin_memset(test_img, 0xAA, BLKDEV_SECTOR_SIZE);
                int rc = ramblk_init(test_img, PAGE_SIZE);
                if (rc >= 0)
                    uart_puts("BLKDEV: ramblk mmcblk0 (test, 8 sectors)\n");
                else
                    uart_puts("BLKDEV: ramblk init FAILED\n");
            } else {
                uart_puts("BLKDEV: page_alloc failed\n");
            }
        }
    }

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

    /* Phase 4 Step 6: mount FAT32 from ramblk if available */
    {
        blkdev_t *sd = blkdev_find("mmcblk0");
        uint32_t fatimg_size = (uint32_t)(__fatimg_end - __fatimg_start);
        if (sd && fatimg_size >= BLKDEV_SECTOR_SIZE) {
            if (vfs_mount("/mnt/sd", &vfat_ops, 0, sd) == 0)
                uart_puts("VFS: vfat mounted at /mnt/sd\n");
            else
                uart_puts("VFS: vfat mount FAILED\n");
        }
    }

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

    /* Phase 4 Step 1: blkdev integration tests */
    blkdev_integration_test();

    /* Phase 4 Step 9: VFAT integration tests */
    vfat_integration_test();

    /* Phase 5 Step 1: loopback block device integration tests */
    loopback_integration_test();

    /* ------------------------------------------------------------------
     * Phase 3 Step 5: exec /bin/test_vfork as the init process (pid 1)
     * Tests vfork + execve + waitpid integration.
     * ------------------------------------------------------------------ */
    proc_table[0].stack_page = page_alloc();

    pcb_t *init = proc_alloc();
    int exec_err = do_execve(init, "/bin/runtests");
    if (exec_err == 0) {
        init->state = PROC_RUNNABLE;
        uart_puts("EXEC: /bin/runtests loaded, pid=");
        uart_print_dec(init->pid);
        uart_puts("\n");
    } else {
        uart_puts("EXEC: /bin/runtests FAILED (err=");
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
