/*
 * uart_rpico.h — RP2040/RP2350 UART extensions
 *
 * Functions for baud rate switching during the 12 MHz → PLL transition.
 * Used by all RP2040/RP2350 targets (pico1, pico1calc, pico2, ...).
 *
 * Boot sequence:
 *   1. uart_init()       — start UART at 115200 @ 12 MHz XOSC
 *   2. uart_tx_drain()   — flush TX ring at 12 MHz baud rate
 *   3. clock_init_pll()  — switch clk_sys to PLL (PPAP_SYS_HZ)
 *   4. uart_reinit_pll() — set PLL-speed baud divisors
 */

#ifndef PPAP_UART_RPICO_H
#define PPAP_UART_RPICO_H

/* Drain the TX ring buffer into the HW FIFO and wait for the shift
 * register to go idle.  Call before clock_init_pll() so all pending
 * bytes are transmitted at the current (12 MHz) baud rate. */
void uart_tx_drain(void);

/* Reconfigure UART0 baud divisors for 115200 @ PPAP_SYS_HZ.
 * Call after clock_init_pll() has switched clk_sys to PLL_SYS. */
void uart_reinit_pll(void);

#endif /* PPAP_UART_RPICO_H */
