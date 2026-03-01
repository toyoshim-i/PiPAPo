/*
 * main.c — Kernel early init entry point
 *
 * Called from Reset_Handler (startup.S) after .data copy and .bss zero.
 * Step 5: UART init + first console output at 12 MHz XOSC.
 * Step 7: PLL_SYS to 133 MHz + UART baud rate update.
 * Step 9: XIP verification — address check, correctness, SysTick benchmark.
 * Phase 1 Steps 1-5: mm_init, proc_init, context switch, SysTick scheduler.
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
#include "smp.h"
#include "xip_test.h"

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

    /* Phase 1 Step 10: wire fd 0/1/2 to the UART tty driver */
    fd_stdio_init(&proc_table[0]);
    uart_puts("FD: fd 0/1/2 wired to UART tty\n");

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
     * Give the kernel init thread (thread 0) its own PSP stack page, then
     * start the round-robin scheduler.  After sched_start() returns, this
     * thread idles with WFI, waking only on interrupts.
     * ------------------------------------------------------------------ */
    proc_table[0].stack_page = page_alloc();

    uart_puts("SCHED: starting preemptive scheduler (10 ms slices @ 133 MHz)\n");

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

    sched_start();

    /* Idle thread — wake on every interrupt, then sleep again. */
    for (;;)
        __asm__ volatile ("wfi");
}
