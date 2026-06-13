/* x68k_logger.c — VFS-side UART/logger for X68000 */

#include "kernel/common/mod/mod_vfs.h"
#include "kernel/vfs/driver/uart.h"
#include "kernel/vfs/driver/uart_x68k.h"
#include "kernel/vfs/klog.h"
#include "kernel/vfs/tty.h"

void klog_init_logger(void) {
  uart_init();
  klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
}

void vfs_notify(int event) {
  switch (event) {
    case VFS_EVENT_MODULE_READY:
      /* Register both klog backends here so target_early_init's klogf
       * output (banner, RTC seed, etc.) reaches the serial mirror, not
       * just TVRAM.  TTY backend wiring stays in LATE_INIT — the TTY
       * subsystem itself comes up later, inside vfs_init. */
      klog_init_logger();
      klog_set_logger(KLOG_LOGGER_SECONDARY, uart_serial_putc, NULL);
      break;
    case VFS_EVENT_LATE_INIT: {
      /* Dual-TTY: TTY_DISPLAY = TVRAM (primary), TTY_SERIAL = RS-232C */
      static const tty_backend_t tvram_be = {
          .putc = uart_putc,
          .getc = uart_getc,
          .rx_avail = uart_rx_avail,
          .get_cols = NULL,
          .get_rows = NULL,
      };
      static const tty_backend_t serial_be = {
          .putc = uart_serial_putc,
          .getc = uart_serial_getc,
          .rx_avail = uart_serial_rx_avail,
          .get_cols = NULL,
          .get_rows = NULL,
      };
      tty_set_backend(TTY_DISPLAY, &tvram_be);
      tty_set_backend(TTY_SERIAL, &serial_be);
      tty_set_console(TTY_DISPLAY);
      break;
    }
    case VFS_EVENT_IDLE:
      /* TVRAM keyboard can be sensed through the MFP directly.  Serial is
       * IOCS-owned here, so idle only polls it when the IOCS mutex is free. */
      if (uart_rx_avail_hw()) tty_rx_notify(TTY_DISPLAY);
      if (uart_serial_rx_avail_idle()) tty_rx_notify(TTY_SERIAL);
      break;
    case VFS_EVENT_CRASH_ENTER:
      /* arch/m68k crash handler will klogf next; stop IOCS TVRAM output
       * so a crash originating inside IOCS itself can't double-fault.
       * Log bytes still reach the _OUT232C serial mirror. */
      uart_tvram_inhibit = true;
      break;
  }
}
