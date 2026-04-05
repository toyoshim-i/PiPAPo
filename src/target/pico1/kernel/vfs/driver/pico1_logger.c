/* pico1_logger.c — VFS-side UART/logger for Pico 1 */

#include "kernel/vfs/klog.h"
#include "kernel/vfs/driver/uart.h"
#include "kernel/vfs/driver/uart_rpico.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/common/config.h"

void klog_init_logger(void) {
  uart_init();
  klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
  klogf("PiPaPo booting... [pico1]\n");
  klogf("UART: 115200 bps @ 12 MHz XOSC\n");
}

void vfs_notify(int event) {
  switch (event) {
    case VFS_EVENT_WILL_PLL_CHANGE:
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
  }
}
