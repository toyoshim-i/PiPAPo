/* pico2rv_logger.c — VFS-side UART/logger for Pico 2 RISC-V */

#include "kernel/vfs/klog.h"
#include "kernel/vfs/uart.h"
#include "kernel/vfs/driver/uart_rp2350.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/common/config.h"

/* UART RX availability check — declared in uart_rp2350.c */
extern int uart_rx_avail(void);

extern void sched_set_input_poll(int (*fn)(void), int tty_idx);

void klog_logger_init(void) {
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
      /* Register UART RX polling for the serial TTY.
       * RISC-V UART is polled (no RX interrupt), so the idle loop's
       * sched_display_poll() checks uart_rx_avail() every 20ms and
       * wakes blocked tty readers when data arrives. */
      sched_set_input_poll(uart_rx_avail, 0 /* TTY_SERIAL */);
      break;
  }
}
