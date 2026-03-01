/*
 * main.c — Kernel early init entry point
 *
 * Called from Reset_Handler (startup.S) after .data copy and .bss zero.
 * Step 5: UART init + first console output at 12 MHz XOSC.
 * Step 7: PLL_SYS to 133 MHz + UART baud rate update.
 * Step 9: XIP verification — address check, correctness, SysTick benchmark.
 * Phase 1 Steps 1-5: mm_init, proc_init, context switch, SysTick scheduler.
 * Phase 1 Step 6: UART0 IRQ mode — uart_init_irq() before sched_start().
 */

#include "drivers/uart.h"
#include "drivers/clock.h"
#include "mm/page.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "xip_test.h"

/* ── Test thread ─────────────────────────────────────────────────────────── */
/*
 * Kernel thread 1: runs concurrently with the kernel init thread (thread 0).
 * Prints "1\n" in a loop so we can see preemptive scheduling in the output.
 * The busy-wait delay gives SysTick time to fire and trigger a context switch.
 */
static void thread1(void)
{
    for (;;) {
        uart_puts("1\n");
        for (volatile uint32_t d = 0u; d < 200000u; d++) {}
    }
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

    /* Phase 1 Step 1: memory manager init + boot-time memory map */
    mm_init();

    /* Phase 1 Step 3: process table init */
    proc_init();

    /* ------------------------------------------------------------------
     * Step 9: XIP verification
     * ------------------------------------------------------------------ */
    uart_puts("XIP: xip_add @ ");
    uart_print_hex32((uint32_t)(uintptr_t)xip_add);
    uart_puts("\n");

    int result = xip_add(3, 4);
    uart_puts("XIP: xip_add(3,4) = ");
    uart_putc('0' + (char)result);
    uart_puts("\n");

    uint32_t flash_cyc = xip_bench(10000);
    uint32_t sram_cyc  = sram_bench(10000);
    uart_puts("XIP: flash bench(10000) = ");
    uart_print_hex32(flash_cyc);
    uart_puts(" cycles\n");
    uart_puts("XIP: sram  bench(10000) = ");
    uart_print_hex32(sram_cyc);
    uart_puts(" cycles\n");

    /* ------------------------------------------------------------------
     * Phase 1 Steps 4+5: context switch + SysTick preemption
     *
     * Set up a second kernel thread, then start the round-robin scheduler.
     * After sched_start() returns, this function continues as thread 0 and
     * will be preempted by SysTick every SYSTICK_RELOAD+1 CPU cycles.
     * ------------------------------------------------------------------ */
    /* Give Thread 0 (the kernel init thread) its own stack page so that its
     * PSP stack is separate from the MSP exception-handler stack.
     * Must be allocated before sched_start() reads proc_table[0].stack_page. */
    proc_table[0].stack_page = page_alloc();

    pcb_t *p1 = proc_alloc();
    p1->stack_page = page_alloc();
    proc_setup_stack(p1, thread1);
    p1->state = PROC_RUNNABLE;

    uart_puts("SCHED: starting preemptive scheduler (10 ms slices @ 133 MHz)\n");

    /* Drain any remaining polling TX, then switch UART0 to IRQ-driven mode.
     * After uart_init_irq() all uart_putc/uart_puts calls from threads are
     * non-blocking ring-buffer writes; UART0_IRQ_Handler does the draining. */
    uart_flush();
    uart_init_irq();

    sched_start();

    /* Thread 0 continues here — print "0\n" in a loop */
    for (;;) {
        uart_puts("0\n");
        for (volatile uint32_t d = 0u; d < 200000u; d++) {}
    }
}
