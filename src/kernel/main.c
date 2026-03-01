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
 */

#include "drivers/uart.h"
#include "drivers/clock.h"
#include "mm/page.h"
#include "mm/mpu.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "fd/fd.h"
#include "vfs/vfs.h"
#include "fs/romfs.h"
#include "fs/devfs.h"
#include "syscall/syscall.h"
#include "smp.h"

/* Linker-provided romfs image location in flash */
extern const uint8_t __romfs_start[];
extern const uint8_t __romfs_end[];

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

    /* Phase 1 Step 1: memory manager init + boot-time memory map */
    mm_init();

    /* Phase 1 Step 3: process table init */
    proc_init();

    /* Phase 2 Steps 1-3: VFS layer + file pool for sys_open */
    vfs_init();
    file_pool_init();

    /* Phase 2 Step 7: mount romfs at / */
    if (vfs_mount("/", &romfs_ops, MNT_RDONLY, __romfs_start) == 0) {
        uart_puts("VFS: romfs mounted at /\n");

        /* Smoke test: read /etc/hostname via VFS path resolution */
        vnode_t *vn = 0;
        if (vfs_lookup("/etc/hostname", &vn) == 0 && vn) {
            char buf[32];
            long n = vn->mount->ops->read(vn, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                uart_puts("ROMFS: /etc/hostname = ");
                uart_puts(buf);
            }
            vnode_put(vn);
        }
    } else {
        uart_puts("VFS: romfs mount FAILED\n");
    }

    /* Phase 2 Step 8: mount devfs at /dev */
    if (vfs_mount("/dev", &devfs_ops, 0, NULL) == 0) {
        uart_puts("VFS: devfs mounted at /dev\n");
    } else {
        uart_puts("VFS: devfs mount FAILED\n");
    }

    /* Phase 1 Step 10: wire fd 0/1/2 to the UART tty driver */
    fd_stdio_init(&proc_table[0]);
    uart_puts("FD: fd 0/1/2 wired to UART tty\n");

    /* ------------------------------------------------------------------
     * Phase 1 Steps 4+5: context switch + SysTick preemption
     *
     * Give the kernel init thread (thread 0) its own PSP stack page, then
     * start the round-robin scheduler.  After sched_start() returns, this
     * thread idles with WFI, waking only on interrupts.
     * ------------------------------------------------------------------ */
    proc_table[0].stack_page = page_alloc();

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
