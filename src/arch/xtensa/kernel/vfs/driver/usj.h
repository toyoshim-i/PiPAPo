/*
 * usj.h — USB Serial JTAG console driver interface (xtensa-only)
 *
 * USB Serial JTAG is the console transport on ESP32-S3 boards whose
 * UART0 pins are not exposed (e.g. M5Stack CardComputer routes those
 * pins to I2S and IR instead).  This is *not* a UART driver — it is
 * a separate transport with its own ROM/HAL layer, so it has its own
 * header rather than re-using `kernel/vfs/driver/uart.h`.
 *
 * The exported function shape mirrors the UART driver because the
 * kernel TTY layer expects the same per-byte interface; the actual
 * registration with the TTY layer happens via `usj_tty_backend`,
 * passed to `tty_set_backend(TTY_SERIAL, ...)` from the target's
 * logger.c.  klog's primary logger is set the same way using
 * `usj_putc`.
 */

#ifndef PPAP_ARCH_XTENSA_KERNEL_VFS_DRIVER_USJ_H
#define PPAP_ARCH_XTENSA_KERNEL_VFS_DRIVER_USJ_H

#include "kernel/vfs/tty.h"

/* ── Init / lifecycle ────────────────────────────────────────────────────── */

void usj_init(void);

/* ── Write ───────────────────────────────────────────────────────────────── */

/* Write one byte to the USJ TX FIFO and immediately request a USB IN
 * packet flush so the host sees the byte without waiting for a packet-
 * boundary or newline trigger.  Returns 1 on success, 0 on host-
 * disconnected timeout (the call is bounded so a missing host can't
 * wedge the kernel).  notify is unused (USJ has no ring buffer that
 * could fill in software). */
int usj_putc(char c, void (*notify)(void));

/* ── Read ────────────────────────────────────────────────────────────────── */

int usj_getc(void);
int usj_rx_avail(void);

/* ── TTY backend ─────────────────────────────────────────────────────────── */

/* Pre-built backend struct that wires usj_* into tty.c's tty_backend_t.
 * Targets register it with `tty_set_backend(TTY_SERIAL, &usj_tty_backend)`. */
extern const tty_backend_t usj_tty_backend;

#endif /* PPAP_ARCH_XTENSA_KERNEL_VFS_DRIVER_USJ_H */
