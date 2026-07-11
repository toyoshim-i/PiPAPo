/*
 * serial_x68k.c — X68000 RS-232C serial console driver via IPL IOCS
 *
 * Secondary tty backend + klog serial mirror.  Output goes through _OUT232C;
 * input is polled via _LOF232C (count) then _INP232C (read).  All calls take
 * the shared x68k IOCS guard, matching the TVRAM console path.
 *
 * The ROM SCC interrupt path owns receive buffering.  Poll _LOF232C instead
 * of SCC RR0 so idle wakeups see bytes already drained into the IOCS buffer;
 * call _INP232C only after _LOF232C reports data (it blocks on an empty ROM
 * buffer).
 *
 * IOCS functions used:
 *   _LOF232C   (d0=0x31)             RS-232C receive buffer count
 *   _INP232C   (d0=0x32)             Input one RS-232C serial character
 *   _OUT232C   (d0=0x35, d1.b=char)  Output one character to RS-232C serial
 */

#include "kernel/vfs/driver/serial_x68k.h"

#include <stdint.h>

#include "kernel/vfs/driver/tvram_x68k.h"
#include "kernel/vfs/driver/x68k_iocs.h"

/* ── IOCS call wrappers ────────────────────────────────────────────────── */

static inline void iocs_out232c(char c) {
  register int32_t d0 asm("d0") = 0x35;
  register int32_t d1 asm("d1") = (unsigned char)c;
  asm volatile("trap #15" : "+r"(d0) : "r"(d1) : "d2", "a0", "a1", "memory");
}

static inline int iocs_lof232c(void) {
  register int32_t d0 asm("d0") = 0x31;
  asm volatile("trap #15" : "+r"(d0) : : "d1", "d2", "a0", "a1", "memory");
  return d0;
}

static inline int iocs_inp232c(void) {
  register int32_t d0 asm("d0") = 0x32;
  asm volatile("trap #15" : "+r"(d0) : : "d1", "d2", "a0", "a1", "memory");
  return d0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int uart_serial_putc(char c, void (*notify)(void)) {
  (void)notify;
  if (uart_tvram_inhibit && x68k_iocs_held_by_current()) return 1;
  x68k_iocs_enter();
  x68k_iocs_irq_state_t irq = x68k_iocs_irq_begin();
  if (c == '\n') iocs_out232c('\r');
  iocs_out232c(c);
  x68k_iocs_irq_end(irq);
  x68k_iocs_exit();
  return 1;
}

int uart_serial_getc(void) {
  x68k_iocs_enter();
  x68k_iocs_irq_state_t irq = x68k_iocs_irq_begin();
  int avail = iocs_lof232c();
  if (avail <= 0) {
    x68k_iocs_irq_end(irq);
    x68k_iocs_exit();
    return -1;
  }
  int c = iocs_inp232c();
  x68k_iocs_irq_end(irq);
  x68k_iocs_exit();
  if (c > 0x7F) return -1;
  return c;
}

int uart_serial_rx_avail(void) {
  x68k_iocs_enter();
  x68k_iocs_irq_state_t irq = x68k_iocs_irq_begin();
  int r = iocs_lof232c() > 0;
  x68k_iocs_irq_end(irq);
  x68k_iocs_exit();
  return r;
}

int uart_serial_rx_avail_idle(void) {
  if (!x68k_iocs_try_enter()) return 0;
  x68k_iocs_irq_state_t irq = x68k_iocs_irq_begin();
  int r = iocs_lof232c() > 0;
  x68k_iocs_irq_end(irq);
  x68k_iocs_exit();
  return r;
}
