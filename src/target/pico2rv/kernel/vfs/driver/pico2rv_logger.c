/* pico2rv_logger.c — VFS-side UART/logger for Pico 2 RISC-V */

#include "kernel/common/config.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/vfs/driver/uart.h"
#include "kernel/vfs/driver/uart_rp2350.h"
#include "kernel/vfs/klog.h"
#include "kernel/vfs/tty.h"

void klog_init_logger(void) {
  /* Idempotent: vfs_notify(WILL_PLL_CHANGE) calls this before clock_init_pll()
   * to enable XOSC, and vfs_init() also calls it later.  uart_init() runs
   * clock_switch_to_xosc() which would knock clk_sys back from PLL to XOSC
   * if invoked twice. */
  static int initialized;
  if (initialized) return;
  initialized = 1;
  uart_init();
  klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
  tty_set_backend(TTY_SERIAL, &uart_tty_backend);
  klogf("PiPAPo booting... [pico2rv]\n");
}

void vfs_notify(int event) {
  switch (event) {
    case VFS_EVENT_WILL_PLL_CHANGE:
      /* uart_init() inside klog_init_logger() enables XOSC, which
       * clock_init_pll() needs as its PLL reference.  Run it before
       * draining TX so the PLL transition has a stable XOSC. */
      klog_init_logger();
      uart_tx_drain();
      break;
    case VFS_EVENT_PLL_CHANGED:
      uart_reinit_pll();
      klogf("System clock: %u MHz\n", PPAP_SYS_HZ / 1000000u);
      break;
    case VFS_EVENT_LATE_INIT:
      while (uart_getc() >= 0)
        ; /* drain boot noise from RX ring */
      break;
    case VFS_EVENT_IDLE:
      tty_poll_input();
      break;
  }
}
