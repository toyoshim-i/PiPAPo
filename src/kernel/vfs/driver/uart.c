/*
 * uart.c — Shared TTY backend wrapper for the UART driver interface.
 *
 * The kernel TTY layer accepts arbitrary serial transports through
 * tty_backend_t.  Targets that drive literal UART hardware (most of
 * the tree — pico1/2/calc/2rv, qemu_arm/m68k/rv32, pcxt) all wrap
 * their per-arch UART driver's uart_putc / uart_getc / uart_rx_avail
 * symbols in the same struct, so define it here once and let those
 * targets register it via:
 *     tty_set_backend(TTY_SERIAL, &uart_tty_backend);
 * from their klog_init_logger().
 *
 * This file is added to a target's CMakeLists.txt only when the
 * target builds with one of the standard UART driver implementations
 * (uart_rpico.c / uart_qemu.c / uart_ns16550.c / uart_com.c / …).
 * xtensa_cc uses USB Serial JTAG instead and provides its own
 * usj_tty_backend in src/arch/xtensa/kernel/vfs/driver/usj.c.  x68k
 * has a TVRAM/RS-232 dual-backend setup wired in its own logger.c
 * and does not consume this file either.
 */

#include "kernel/vfs/driver/uart.h"

const tty_backend_t uart_tty_backend = {
    .putc = uart_putc,
    .flush = NULL,
    .getc = uart_getc,
    .rx_avail = uart_rx_avail,
    .get_cols = NULL,
    .get_rows = NULL,
    .set_winsize = NULL,
};
