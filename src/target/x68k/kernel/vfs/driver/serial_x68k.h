/*
 * serial_x68k.h — X68000 RS-232C serial console driver (secondary TTY)
 *
 * Drives the RS-232C port via IOCS (_OUT232C / _LOF232C / _INP232C) for the
 * secondary (serial) tty and the klog serial mirror.
 */

#ifndef PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_SERIAL_X68K_H
#define PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_SERIAL_X68K_H

int uart_serial_putc(char c, void (*notify)(void));
int uart_serial_getc(void);
int uart_serial_rx_avail(void);

/* Availability check used by the idle notifier.  Senses via IOCS and returns
 * 0 without polling when the IOCS mutex is busy. */
int uart_serial_rx_avail_idle(void);

#endif /* PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_SERIAL_X68K_H */
