/*
 * uart_x68k.h -- Public hooks exported by the X68000 UART / IOCS driver
 */

#ifndef PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_UART_X68K_H
#define PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_UART_X68K_H

#include <stdbool.h>

/* Crash-safe flag.  When set, uart_putc skips the TVRAM / _B_PUTC path
 * so a crash occurring inside IOCS itself cannot double-fault on the
 * way to the log.  Raised by x68k's vfs_notify(VFS_EVENT_CRASH_ENTER)
 * handler when the m68k crash path enters, so subsequent klogf output
 * is forced to the _OUT232C serial mirror. */
extern bool uart_tvram_inhibit;

/* ── RS-232C serial backend (secondary TTY) ────────────────────────────── */

int uart_serial_putc(char c, void (*notify)(void));
int uart_serial_getc(void);
int uart_serial_rx_avail(void);

/* Non-IOCS availability checks — safe from timer-ISR context where IOCS
 * is not re-entrant (IOCS _B_PUTC lowers IPL internally for VSYNC sync). */
int uart_rx_avail_hw(void);        /* MFP USART RSR bit 7 */
int uart_serial_rx_avail_hw(void); /* SCC channel B RR0 bit 0 */

#endif /* PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_UART_X68K_H */
