/* x68k_logger.c — VFS-side UART/logger for X68000 */

#include "kernel/vfs/klog.h"
#include "kernel/vfs/uart.h"
#include "kernel/vfs/tty.h"
#include "kernel/common/mod/mod_vfs.h"

extern int uart_serial_putc(char c, void (*notify)(void));
extern int uart_serial_getc(void);
extern int uart_serial_rx_avail(void);
extern int uart_rx_avail_hw(void);
extern int uart_serial_rx_avail_hw(void);

extern void sched_set_input_poll(int (*fn)(void), int tty_idx);
extern void sched_set_input_poll2(int (*fn)(void), int tty_idx);

void klog_logger_init(void) {
  uart_init();
  klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
}

void vfs_notify(int event) {
  if (event != VFS_EVENT_LATE_INIT) return;

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
  klog_set_logger(KLOG_LOGGER_SECONDARY, uart_serial_putc, NULL);

  /* Register input polls for both consoles.
   * Both use direct hardware register reads — NO IOCS TRAP #15 calls —
   * because IOCS functions hang when called from the idle thread context. */
  sched_set_input_poll(uart_rx_avail_hw, TTY_DISPLAY);
  sched_set_input_poll2(uart_serial_rx_avail_hw, TTY_SERIAL);
}
