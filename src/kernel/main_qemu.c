/*
 * main_qemu.c — Kernel entry point for QEMU mps2-an500 smoke test
 *
 * Replaces main.c for the ppap_qemu build target.  Exercises the same
 * kernel logic as main.c but adapts for QEMU's constraints:
 *
 *   - No clock_init_pll()      (QEMU has no PLL or XOSC to configure)
 *   - No uart_reinit_133mhz()  (clock never changes)
 *   - UART via CMSDK UART0     (uart_qemu.c, not uart.c)
 *   - xip_add() still works    (pure integer arithmetic, no hardware)
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
#include "smp.h"
#include "xip_test.h"

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

    /* Phase 1 Step 10: wire fd 0/1/2 to the UART tty driver */
    fd_stdio_init(&proc_table[0]);

    /* xip_add is pure arithmetic — runs identically in QEMU and on hardware */
    uart_puts("XIP: xip_add @ ");
    uart_print_hex32((uint32_t)(uintptr_t)xip_add);
    uart_puts(" (ROM, 0x000xxxxx in QEMU)\n");

    int result = xip_add(3, 4);
    uart_puts("XIP: xip_add(3,4) = ");
    uart_putc('0' + (char)result);
    uart_puts("\n");

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
