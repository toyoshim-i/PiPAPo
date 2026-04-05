/* pico2_logger.c — VFS-side UART/logger for Pico 2 */

#include "kernel/vfs/klog.h"
#include "kernel/vfs/driver/uart.h"
#include "kernel/vfs/driver/uart_rpico.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/common/config.h"

void klog_init_logger(void) {
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
