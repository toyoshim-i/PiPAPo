/*
 * uart.h — UART0 driver interface
 *
 * uart_init_console() initializes UART0 at 115200 8N1 on GPIO 0/1 with the
 * system clock on the 12 MHz XOSC.  After clock_init_pll() switches to
 * 133 MHz, call uart_flush() then uart_reinit_133mhz() to restore the
 * correct baud rate.
 */

#ifndef PPAP_UART_H
#define PPAP_UART_H

#include <stdint.h>

/* Initialize UART0 console (115200 8N1, GPIO 0=TX / GPIO 1=RX).
 * Also switches clk_sys from ROSC to 12 MHz XOSC.
 * Must be called before any uart_putc / uart_puts calls. */
void uart_init_console(void);

/* Blocking single-character output. */
void uart_putc(char c);

/* Blocking string output. '\n' is expanded to '\r\n'. */
void uart_puts(const char *s);

/* Block until the TX FIFO is empty and the shift register is idle.
 * Call this before clock_init_pll() to avoid mid-byte corruption. */
void uart_flush(void);

/* Update baud rate divisors for 115200 bps at 133 MHz (after clock_init_pll()).
 * Briefly disables and re-enables the UART. */
void uart_reinit_133mhz(void);

/* Print a 32-bit value as "0xXXXXXXXX" (8 lowercase hex digits, no newline). */
void uart_print_hex32(uint32_t v);

#endif /* PPAP_UART_H */
