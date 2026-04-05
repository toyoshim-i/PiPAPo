/* xtensa_cc_logger.c — VFS-side UART/logger for CardComputer */

#include "kernel/common/config.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/vfs/driver/uart.h"
#include "kernel/vfs/klog.h"

extern void sched_set_input_poll(int (*fn)(void), int tty_idx);

void klog_init_logger(void) {
  /* ESP-IDF already initializes UART0 during ROM boot — no uart_init needed */
  klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
  klogf("PiPaPo booting... [xtensa_cc]\n");
  klogf("System clock: %lu MHz\n", (unsigned long)(PPAP_SYS_HZ / 1000000u));
}

void vfs_notify(int event) {
  if (event == VFS_EVENT_LATE_INIT)
    sched_set_input_poll(uart_rx_avail, 0 /* TTY_SERIAL */);
}
