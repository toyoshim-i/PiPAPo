/*
 * clock.h — System clock driver interface
 *
 * clock_init_pll() configures PLL_SYS to 133 MHz from the 12 MHz XOSC
 * and switches clk_sys to use it.  Call uart_flush() before this and
 * uart_reinit_133mhz() immediately after to keep the UART baud rate correct.
 */

#ifndef PPAP_CLOCK_H
#define PPAP_CLOCK_H

/* Configure PLL_SYS for 133 MHz and switch clk_sys to it.
 * Prerequisite: XOSC must already be running (uart_init_console() does this).
 * After this call clk_sys = clk_peri = 133 MHz. */
void clock_init_pll(void);

#endif /* PPAP_CLOCK_H */
