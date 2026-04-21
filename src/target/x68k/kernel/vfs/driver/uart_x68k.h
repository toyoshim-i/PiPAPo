/*
 * uart_x68k.h -- Public hooks exported by the X68000 UART / IOCS driver
 */

#ifndef PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_UART_X68K_H
#define PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_UART_X68K_H

/* Crash-safe flag.  When set, uart_putc skips the TVRAM / _B_PUTC path
 * so a crash occurring inside IOCS itself cannot double-fault on the
 * way to the log.  Raised by x68k's vfs_notify(VFS_EVENT_CRASH_ENTER)
 * handler when the m68k crash path enters, so subsequent klogf output
 * is forced to the _OUT232C serial mirror. */
extern int uart_tvram_inhibit;

#endif /* PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_UART_X68K_H */
