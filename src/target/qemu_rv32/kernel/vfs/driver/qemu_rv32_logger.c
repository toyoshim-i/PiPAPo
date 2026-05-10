/* qemu_rv32_logger.c — VFS-side UART/logger for QEMU RISC-V */

#include "kernel/common/config.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/vfs/driver/uart.h"
#include "kernel/vfs/klog.h"
#include "kernel/vfs/tty.h"

void klog_init_logger(void) {
  uart_init();
  klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
  klogf("PiPAPo booting... [qemu_rv32]\n");
  klogf("System clock: %lu MHz\n", (unsigned long)(PPAP_SYS_HZ / 1000000u));
}

void vfs_notify(int event) {
  if (event == VFS_EVENT_LATE_INIT) {
    while (uart_getc() >= 0)
      ; /* drain boot noise */
  } else if (event == VFS_EVENT_IDLE) {
    tty_poll_input();
  }
}
