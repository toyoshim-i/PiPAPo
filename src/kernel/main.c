/*
 * main.c — Kernel early init entry point
 *
 * Called from Reset_Handler (startup.S) after .data copy and .bss zero.
 * Step 5: UART init + first console output at 12 MHz XOSC.
 * Step 7: PLL_SYS to 133 MHz + UART baud rate update.
 * Phase 1 Steps 1-5: mm_init (incl. XIP verify+bench), proc_init, context switch, SysTick scheduler.
 * Phase 1 Step 6: UART0 IRQ mode — uart_init_irq() before sched_start().
 * Phase 1 Step 10: fd_stdio_init() — fd 0/1/2 wired to UART tty driver.
 * Phase 1 Step 11: mpu_init() — 4-region MPU layout; mpu_switch() on context switch.
 * Phase 1 Step 12: core1_launch() — start Core 1 running the SIO echo worker.
 * Phase 2 Step 10: syscall-level VFS integration test.
 */

#include "drivers/uart.h"
#include "drivers/clock.h"
#include "drivers/spi.h"
#include "drivers/sd.h"
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
#include "fs/tmpfs.h"
#include "syscall/syscall.h"
#include "exec/exec.h"
#include "blkdev/blkdev.h"
#include "blkdev/loopback.h"
#include "fs/vfat.h"
#include "fs/fstab.h"
#include "errno.h"
#include "smp.h"

/* Linker-provided romfs image location in flash */
extern const uint8_t __romfs_start[];
extern const uint8_t __romfs_end[];

/* ── Syscall-level integration test ───────────────────────────────────────── */

static void vfs_integration_test(void)
{
    uart_puts("VFS: integration test ... ");
    int ok = 1;

    /* open + read /etc/hostname via syscall layer */
    {
        long fd = sys_open("/etc/hostname", O_RDONLY, 0);
        if (fd >= 0) {
            char buf[32];
            long n = sys_read(fd, buf, sizeof(buf) - 1);
            if (n == 5) {
                buf[n] = '\0';
                uart_puts(buf);
            } else {
                ok = 0;
            }
            sys_close(fd);
        } else {
            ok = 0;
        }
    }

    /* open nonexistent file → -ENOENT */
    {
        long fd = sys_open("/nonexistent", O_RDONLY, 0);
        if (fd != -(long)ENOENT)
            ok = 0;
    }

    /* open + write /dev/null */
    {
        long fd = sys_open("/dev/null", O_WRONLY, 0);
        if (fd >= 0) {
            long n = sys_write(fd, "discarded", 9);
            if (n != 9) ok = 0;
            sys_close(fd);
        } else {
            ok = 0;
        }
    }

    /* open + read /dev/zero → all zero bytes */
    {
        long fd = sys_open("/dev/zero", O_RDONLY, 0);
        if (fd >= 0) {
            char buf[4] = {0xFF, 0xFF, 0xFF, 0xFF};
            long n = sys_read(fd, buf, 4);
            if (n != 4 || buf[0] || buf[1] || buf[2] || buf[3])
                ok = 0;
            sys_close(fd);
        } else {
            ok = 0;
        }
    }

    /* open + read /proc/meminfo */
    {
        long fd = sys_open("/proc/meminfo", O_RDONLY, 0);
        if (fd >= 0) {
            char buf[128];
            long n = sys_read(fd, buf, sizeof(buf) - 1);
            if (n <= 0) ok = 0;
            sys_close(fd);
        } else {
            ok = 0;
        }
    }

    /* stat /etc/hostname → regular file */
    {
        struct stat st;
        long rc = sys_stat("/etc/hostname", &st);
        if (rc != 0 || !S_ISREG(st.st_mode) || st.st_size != 5)
            ok = 0;
    }

    /* chdir + getcwd */
    {
        long rc = sys_chdir("/etc");
        if (rc == 0) {
            char buf[64];
            long n = sys_getcwd(buf, sizeof(buf));
            if (n <= 0 || __builtin_strcmp(buf, "/etc") != 0)
                ok = 0;
        } else {
            ok = 0;
        }
        sys_chdir("/");
    }

    uart_puts(ok ? "PASS\n" : "FAIL\n");
}

/* ── Kernel entry point ──────────────────────────────────────────────────── */

void kmain(void)
{
    /* Bring up UART at 12 MHz XOSC first so we can print during init */
    uart_init_console();
    uart_puts("PicoPiAndPortable booting...\n");
    uart_puts("UART: 115200 bps @ 12 MHz XOSC\n");

    /* Drain the TX FIFO before changing clk_peri — a mid-byte clock switch
     * would corrupt the character currently in the shift register. */
    uart_flush();

    /* Switch PLL_SYS to 133 MHz; clk_sys and clk_peri follow */
    clock_init_pll();

    /* Reconfigure UART baud rate for the new 133 MHz clock */
    uart_reinit_133mhz();

    uart_puts("System clock: 133 MHz\n");

    /* Phase 4 Step 2: SPI0 at 400 kHz for SD card init sequence */
    spi_init(400000);
    uart_puts("SPI0: initialised at 400 kHz\n");

    /* Phase 1 Step 1: memory manager init + boot-time memory map */
    mm_init();

    /* Phase 1 Step 3: process table init */
    proc_init();

    /* Phase 2 Steps 1-3: VFS layer + file pool for sys_open */
    vfs_init();
    file_pool_init();

    /* Phase 4 Step 1: block device registry */
    blkdev_init();

    /* Phase 5 Step 1: loopback block device subsystem */
    loopback_init();

    /* Phase 4 Step 3: SD card init (skipped gracefully if no card) */
    {
        int rc = sd_init();
        if (rc == 0)
            uart_puts("SD: card initialised, mmcblk0 registered\n");
        else if (rc == -ENODEV)
            uart_puts("SD: no card detected (skipping)\n");
        else {
            uart_puts("SD: init failed (err=");
            uart_print_dec((uint32_t)(-(int)rc));
            uart_puts(")\n");
        }
    }

    /* Bootstrap: mount romfs at / (needed to read /etc/fstab) */
    if (vfs_mount("/", &romfs_ops, MNT_RDONLY, __romfs_start) == 0)
        uart_puts("VFS: romfs mounted at /\n");
    else
        uart_puts("VFS: romfs mount FAILED\n");

    /* Phase 5 Step 10: parse /etc/fstab and mount all entries */
    {
        fstab_entry_t fstab[FSTAB_MAX_ENTRIES];
        int nfstab = fstab_parse(fstab, FSTAB_MAX_ENTRIES);
        if (nfstab > 0) {
            uart_puts("fstab: ");
            uart_print_dec((uint32_t)nfstab);
            uart_puts(" entries parsed\n");
            fstab_mount_all(fstab, nfstab);
        } else {
            uart_puts("fstab: no entries (fallback not implemented)\n");
        }
    }

    /* Phase 1 Step 10: wire fd 0/1/2 to the UART tty driver */
    fd_stdio_init(&proc_table[0]);
    uart_puts("FD: fd 0/1/2 wired to UART tty\n");

    /* Phase 2 Step 10: syscall-level VFS integration test */
    vfs_integration_test();

    /* ------------------------------------------------------------------
     * Phase 1 Steps 4+5: context switch + SysTick preemption
     *
     * Give the kernel init thread (thread 0) its own PSP stack page, then
     * start the round-robin scheduler.  After sched_start() returns, this
     * thread idles with WFI, waking only on interrupts.
     * ------------------------------------------------------------------ */
    proc_table[0].stack_page = page_alloc();

    /* ------------------------------------------------------------------
     * Phase 6 Step 8: launch /sbin/init (busybox) as PID 1
     * Fallback to /bin/sh if init is missing.
     * ------------------------------------------------------------------ */
    {
        pcb_t *init = proc_alloc();
        init->pgid = init->pid;
        init->sid  = init->pid;

        int exec_err = do_execve(init, "/sbin/init");
        if (exec_err < 0) {
            uart_puts("INIT: /sbin/init failed, trying /bin/sh\n");
            exec_err = do_execve(init, "/bin/sh");
        }
        if (exec_err == 0) {
            init->state = PROC_RUNNABLE;
            uart_puts("INIT: pid=");
            uart_print_dec(init->pid);
            uart_puts(" loaded\n");
        } else {
            uart_puts("PANIC: no init or shell (err=");
            uart_print_dec((uint32_t)(-(int)exec_err));
            uart_puts(")\n");
        }
    }

    /* Drain any remaining polling TX, then switch UART0 to IRQ-driven mode.
     * After uart_init_irq() all uart_putc/uart_puts calls are non-blocking
     * ring-buffer writes; UART0_IRQ_Handler does the draining. */
    uart_flush();
    uart_init_irq();
    uart_puts("UART: switched to interrupt-driven mode\n");

    /* Phase 1 Step 11: configure MPU regions and enable memory protection */
    mpu_init();

    /* Phase 1 Step 12: launch Core 1 running the SIO FIFO echo worker */
    core1_launch(core1_io_worker);

    uart_puts("SCHED: starting preemptive scheduler (10 ms slices @ 133 MHz)\n");
    sched_start();

    /* Idle thread — wake on every interrupt, then sleep again. */
    for (;;)
        __asm__ volatile ("wfi");
}
