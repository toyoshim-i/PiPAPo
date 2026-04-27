/* pico2_logger.c — VFS-side UART/logger for Pico 2 */

#include "kernel/common/config.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/vfs/driver/uart.h"
#include "kernel/vfs/driver/uart_rpico.h"
#include "kernel/vfs/klog.h"

void klog_init_logger(void) {
  /* Idempotent: vfs_notify(WILL_PLL_CHANGE) calls this before clock_init_pll()
   * to enable XOSC, and vfs_init() also calls it later.  uart_init() runs
   * clock_switch_to_xosc() which would knock clk_sys back from PLL to XOSC
   * if invoked twice. */
  static int initialized;
  if (initialized) return;
  initialized = 1;
#ifndef PPAP_SEMIHOST
  uart_init();
#endif
  klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
  klogf("PiPAPo booting... [pico2]\n");
#ifndef PPAP_SEMIHOST
  klogf("UART: 115200 bps @ 12 MHz XOSC\n");
#endif
}

void vfs_notify(int event) {
  switch (event) {
    case VFS_EVENT_WILL_PLL_CHANGE:
      /* uart_init() inside klog_init_logger() enables XOSC, which
       * clock_init_pll() needs as its PLL reference.  Run it before
       * draining TX so the PLL transition has a stable XOSC. */
      klog_init_logger();
#ifndef PPAP_SEMIHOST
      uart_tx_drain();
#endif
      break;
    case VFS_EVENT_PLL_CHANGED:
#ifndef PPAP_SEMIHOST
      uart_reinit_pll();
#endif
      klogf("System clock: %u MHz\n", PPAP_SYS_HZ / 1000000u);
      break;
    case VFS_EVENT_LATE_INIT:
      while (uart_getc() >= 0)
        ; /* drain boot noise from RX ring */
      break;
  }
}
