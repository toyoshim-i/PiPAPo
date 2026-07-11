/*
 * tvram_x68k.h — X68000 TVRAM text console driver (public hooks)
 *
 * Implements the shared uart.h console interface (uart_init / uart_putc /
 * uart_getc / uart_rx_avail) on the built-in TVRAM text plane via IOCS.
 * Declared here are the extra x68k-specific hooks not in uart.h.
 */

#ifndef PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_TVRAM_X68K_H
#define PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_TVRAM_X68K_H

#include <stdbool.h>

/* Crash-safe flag.  When set, uart_putc skips the TVRAM / _B_PUTC path so a
 * crash occurring inside IOCS itself cannot double-fault on the way to the
 * log.  Raised by x68k's vfs_notify(VFS_EVENT_CRASH_ENTER) handler when the
 * m68k crash path enters, so subsequent klogf output is forced to the serial
 * mirror.  The serial path consults it too, for its IOCS re-entrancy guard. */
extern bool uart_tvram_inhibit;

/* Keyboard availability check for the idle notifier.  Senses via IOCS and
 * returns 0 without polling when the IOCS mutex is busy. */
int uart_rx_avail_idle(void);

/* Text console geometry (columns/rows), queried from IOCS _B_CONSOL at init.
 * Wired into the display tty backend's get_cols/get_rows hooks so TIOCGWINSZ
 * reflects the real TVRAM text area. */
int uart_get_cols(void);
int uart_get_rows(void);

#endif /* PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_TVRAM_X68K_H */
