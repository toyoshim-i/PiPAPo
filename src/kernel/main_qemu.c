/*
 * main_qemu.c — Kernel entry point for QEMU mps2-an500 smoke test
 *
 * Replaces main.c for the ppap_qemu build target.  Exercises the same
 * kernel logic as main.c but adapts for QEMU's constraints:
 *
 *   - No clock_init_pll()      (QEMU has no PLL or XOSC to configure)
 *   - No uart_reinit_133mhz()  (clock never changes)
 *   - UART via CMSDK UART0     (uart_qemu.c, not uart.c)
 *   - xip_verify() still runs  (address/correctness probe + benches via SysTick)
 *   - Cooperative sched_yield() instead of SysTick preemption
 *     (QEMU mps2-an500 runs the SysTick counter but never asserts TICKINT;
 *      we use explicit sched_yield() calls to trigger PendSV instead)
 *
 * Phase 1 Steps 4+5: PendSV context switch verified on QEMU.
 * Expected output: interleaved "0\n" and "1\n" — proves that PendSV_Handler
 * correctly saves and restores per-thread stacks.
 * (On real hardware SysTick drives preemptive scheduling; see main.c.)
 *
 * Phase 1 Step 11: mpu_init() self-stubs on QEMU (MPU_TYPE == 0).
 * mpu_switch() in switch.S is also a no-op via the mpu_present flag.
 * Phase 1 Step 12: core1_launch() self-stubs on QEMU (SIO_FIFO_ST.RDY == 0).
 */

#include "drivers/uart.h"
#include "mm/page.h"
#include "mm/mpu.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "fd/fd.h"
#include "vfs/vfs.h"
#include "fs/romfs.h"
#include "fs/devfs.h"
#include "fs/procfs.h"
#include "syscall/syscall.h"
#include "smp.h"

/* Linker-provided romfs image location */
extern const uint8_t __romfs_start[];
extern const uint8_t __romfs_end[];

/* ── Test thread ─────────────────────────────────────────────────────────── */

static void thread1(void)
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

    /* Phase 2 Step 9: mount procfs at /proc */
    if (vfs_mount("/proc", &procfs_ops, MNT_RDONLY, NULL) == 0) {
        uart_puts("VFS: procfs mounted at /proc\n");

        /* Smoke test: read /proc/version */
        vnode_t *vn = 0;
        if (vfs_lookup("/proc/version", &vn) == 0 && vn) {
            char buf[64];
            long n = vn->mount->ops->read(vn, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                uart_puts("PROCFS: ");
                uart_puts(buf);
            }
            vnode_put(vn);
        }
    } else {
        uart_puts("VFS: procfs mount FAILED\n");
    }

    /* Phase 1 Step 10: wire fd 0/1/2 to the UART tty driver */
    fd_stdio_init(&proc_table[0]);

    /* ------------------------------------------------------------------
     * Phase 1 Steps 4+5: context switch via cooperative sched_yield()
     *
     * Each thread calls sched_yield() which sets PENDSVSET.  PendSV_Handler
     * saves the caller's callee-saved registers onto its PSP stack, calls
     * sched_next() to select the next RUNNABLE thread, then restores the
     * new thread's saved context.  Interleaved "0\n" / "1\n" output proves
     * that both threads run and that their stacks are isolated.
     * ------------------------------------------------------------------ */
    /* Give Thread 0 (the kernel init thread) its own stack page so that its
     * PSP stack is separate from the MSP exception-handler stack.
     * Must be allocated before sched_start() reads proc_table[0].stack_page. */
    proc_table[0].stack_page = page_alloc();

    pcb_t *p1 = proc_alloc();
    p1->stack_page = page_alloc();
    proc_setup_stack(p1, thread1);
    p1->state = PROC_RUNNABLE;

    /* Phase 1 Step 11: configure MPU (no-op on QEMU — MPU_TYPE reads 0) */
    mpu_init();

    /* Phase 1 Step 12: launch Core 1 (no-op on QEMU — SIO not mapped) */
    core1_launch(core1_io_worker);

    uart_puts("SCHED: starting cooperative context-switch test (QEMU)\n");
    sched_start();

    /* Thread 0 continues here — print "0\n" and yield each iteration */
    for (;;) {
        uart_puts("0\n");
        sched_yield();
    }
}
