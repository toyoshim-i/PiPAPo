/*
 * uart.h — UART0 driver interface
 *
 * uart_init() sets up UART0 at 115200 8N1.  The ISR drains the TX
 * ring → HW FIFO and captures RX bytes into a ring buffer.
 *
 * Before starting user processes, drain boot noise from the RX ring:
 *   while (uart_getc() >= 0) ;
 */

#ifndef PPAP_KERNEL_VFS_DRIVER_UART_H
#define PPAP_KERNEL_VFS_DRIVER_UART_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/vfs/tty.h"

/* ── Init / lifecycle ────────────────────────────────────────────────────────
 */

void uart_init(void);

/* ── Write ───────────────────────────────────────────────────────────────────
 */

/* Enqueue one byte into the TX ring buffer.
 * Returns 1 on success, 0 if ring is full.
 * If 0 and notify != NULL, notify is registered atomically and will
 * be called from the UART ISR when ring space becomes available. */
int uart_putc(char c, void (*notify)(void));

/* ── Read ────────────────────────────────────────────────────────────────────
 */

int uart_getc(void);
int uart_rx_avail(void);

/* ── TTY backend ─────────────────────────────────────────────────────────────
 *
 * Pre-built tty_backend_t wrapping uart_putc / uart_getc / uart_rx_avail.
 * Defined once in src/kernel/vfs/driver/uart.c, which UART-using targets
 * link via their CMakeLists alongside the per-arch driver implementation
 * (uart_rpico.c / uart_qemu.c / uart_ns16550.c / …).  Targets register
 * the serial TTY with one line:
 *     tty_set_backend(TTY_SERIAL, &uart_tty_backend);
 * xtensa_cc uses USB Serial JTAG instead and provides its own
 * usj_tty_backend (see src/arch/xtensa/kernel/vfs/driver/usj.h).  x68k
 * has a TVRAM/RS-232 dual-backend setup wired in its own logger.c and
 * does not link uart.c. */
extern const tty_backend_t uart_tty_backend;

#endif /* PPAP_KERNEL_VFS_DRIVER_UART_H */
