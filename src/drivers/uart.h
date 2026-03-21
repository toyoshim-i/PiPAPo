/*
 * uart.h — UART0 driver interface
 *
 * uart_init() sets up UART0 at 115200 8N1.  The ISR drains the TX
 * ring → HW FIFO and captures RX bytes into a ring buffer.
 *
 * Before starting user processes, drain boot noise from the RX ring:
 *   while (uart_getc() >= 0) ;
 */

#ifndef PPAP_DRIVERS_UART_H
#define PPAP_DRIVERS_UART_H

#include <stdint.h>
#include <stddef.h>

/* ── Init / lifecycle ──────────────────────────────────────────────────────── */

void uart_init(void);

/* ── Write ─────────────────────────────────────────────────────────────────── */

/* Enqueue one byte into the TX ring buffer.
 * Returns 1 on success, 0 if ring is full.
 * If 0 and notify != NULL, notify is registered atomically and will
 * be called from the UART ISR when ring space becomes available. */
int uart_putc(char c, void (*notify)(void));

/* ── Read ──────────────────────────────────────────────────────────────────── */

int uart_getc(void);
int uart_rx_avail(void);

#endif /* PPAP_DRIVERS_UART_H */
