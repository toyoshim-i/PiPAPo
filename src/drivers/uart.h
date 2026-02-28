/*
 * uart.h — UART driver interface (Phase 0 stub)
 * Will be implemented in Step 5.
 */

#ifndef PPAP_UART_H
#define PPAP_UART_H

void uart_init_console(void);
void uart_putc(char c);
void uart_puts(const char *s);

#endif /* PPAP_UART_H */
