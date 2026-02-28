/*
 * uart.h — UART0 driver interface
 *
 * uart_init_console() initializes UART0 at 115200 8N1 on GPIO 0/1.
 * It also switches the system clock from ROSC to the 12 MHz crystal
 * oscillator (XOSC) so the baud rate divisor is exact.
 * Step 7 will extend this to 133 MHz via PLL.
 */

#ifndef PPAP_UART_H
#define PPAP_UART_H

/* Initialize UART0 console (115200 8N1, GPIO 0=TX / GPIO 1=RX).
 * Must be called before any uart_putc / uart_puts calls. */
void uart_init_console(void);

/* Blocking single-character output. */
void uart_putc(char c);

/* Blocking string output. '\n' is expanded to '\r\n'. */
void uart_puts(const char *s);

#endif /* PPAP_UART_H */
