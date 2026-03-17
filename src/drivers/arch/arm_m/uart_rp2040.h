/*
 * uart_rp2040.h — RP2040-specific UART extensions
 *
 * Functions for baud rate switching during the 12 MHz → 133 MHz PLL
 * transition.  Only used by pico1 and pico1calc targets.
 *
 * Boot sequence:
 *   1. uart_init()           — start UART at 115200 @ 12 MHz XOSC
 *   2. uart_tx_drain()       — flush TX ring at 12 MHz baud rate
 *   3. clock_init_pll()      — switch clk_sys to 133 MHz PLL
 *   4. uart_reinit_133mhz()  — set 133 MHz baud divisors
 */

#ifndef PPAP_UART_RP2040_H
#define PPAP_UART_RP2040_H

/* Drain the TX ring buffer into the HW FIFO and wait for the shift
 * register to go idle.  Call before clock_init_pll() so all pending
 * bytes are transmitted at the current (12 MHz) baud rate. */
void uart_tx_drain(void);

/* Reconfigure UART0 baud divisors for 115200 @ 133 MHz.
 * Call after clock_init_pll() has switched clk_sys to PLL_SYS. */
void uart_reinit_133mhz(void);

#endif /* PPAP_UART_RP2040_H */
