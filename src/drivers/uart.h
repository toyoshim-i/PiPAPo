/*
 * uart.h — UART0 driver interface
 *
 * Polling mode (before uart_init_irq):
 *   uart_init_console() initializes UART0 at 115200 8N1 on GPIO 0/1 with the
 *   system clock on the 12 MHz XOSC.  After clock_init_pll() switches to
 *   133 MHz, call uart_flush() then uart_reinit_133mhz() to restore the
 *   correct baud rate.  uart_putc/uart_puts spin until the TX FIFO has room.
 *
 * IRQ mode (after uart_init_irq):
 *   uart_putc/uart_puts write into a 255-byte TX ring buffer and return
 *   immediately.  UART0_IRQ_Handler drains the ring into the TX FIFO.
 *   Incoming bytes are captured into a 64-byte RX ring buffer; uart_getc()
 *   reads from it.  Call uart_init_irq() once, just before sched_start().
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

/* Print a 32-bit value as an unsigned decimal integer (no newline). */
void uart_print_dec(uint32_t v);

/* Switch UART0 TX and RX to interrupt-driven mode.
 * Must be called exactly once, just before sched_start().
 * After this call uart_putc/uart_puts are non-blocking ring-buffer writes
 * and UART0_IRQ_Handler drains them asynchronously.
 * On QEMU this is a no-op (CMSDK UART stays in polling mode). */
void uart_init_irq(void);

/* Non-blocking RX read.
 * Returns the next byte from the RX ring buffer, or -1 if none is available.
 * On QEMU reads the CMSDK UART DATA register if RX is ready. */
int uart_getc(void);

/* Returns 1 if the RX ring buffer has data available, 0 otherwise.
 * Non-blocking, does not consume data.  Used by tty_poll(). */
int uart_rx_avail(void);

#endif /* PPAP_UART_H */
