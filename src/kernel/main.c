/*
 * main.c — Kernel early init entry point
 *
 * Called from Reset_Handler (startup.S) after .data copy and .bss zero.
 * Step 5: UART init + first console output at 12 MHz XOSC.
 * Step 7: PLL_SYS to 133 MHz + UART baud rate update.
 * Step 9: XIP verification — address check, correctness, SysTick benchmark.
 */

#include "drivers/uart.h"
#include "drivers/clock.h"
#include "mm/page.h"
#include "xip_test.h"

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

    /* ------------------------------------------------------------------
     * Step 9: XIP verification
     *
     * 1. Print xip_add address — must be in 0x10001xxx (XIP flash range).
     * 2. Call xip_add(3,4) — must return 7 (proves XIP execution works).
     * 3. Benchmark the same loop running from flash (xip_bench) vs SRAM
     *    (sram_bench, copied by Reset_Handler).  Flash should be within
     *    a small multiple of SRAM speed, confirming cache hits.
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

    for (;;) {
    }
}
