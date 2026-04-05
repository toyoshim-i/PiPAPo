/* pico2rv_logger.c — VFS-side UART/logger for Pico 2 RISC-V */

#include "kernel/common/config.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/vfs/driver/uart.h"
#include "kernel/vfs/driver/uart_rp2350.h"
#include "kernel/vfs/klog.h"

void klog_init_logger(void) {
  uart_init();
  klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
  klogf("PiPaPo booting... [pico2rv]\n");
}

void vfs_notify(int event) {
  switch (event) {
    case VFS_EVENT_WILL_PLL_CHANGE:
      /* On Hazard3, UART_FR.BUSY can remain set spuriously and hang boot
       * if we spin on uart_tx_drain() before the first write.  Skip. */
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
