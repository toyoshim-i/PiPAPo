/*
 * main.c — Kernel early init entry point
 *
 * Called from Reset_Handler (startup.S) after .data copy and .bss zero.
 * Step 5: UART init + first console output at 12 MHz XOSC.
 * Step 7: will add PLL setup for 133 MHz and reinit UART.
 * Step 9: will add XIP verification output.
 */

#include "drivers/uart.h"

void kmain(void)
{
    uart_init_console();
    uart_puts("PicoPiAndPortable booting...\n");
    uart_puts("UART: 115200 bps @ 12 MHz XOSC\n");

    /* Step 7: clock_init_pll() + uart_reinit_133mhz() go here */

    for (;;) {
    }
}
