/* qemu_arm_logger.c — VFS-side UART/logger for QEMU ARM */

#include "kernel/vfs/klog.h"
#include "kernel/vfs/uart.h"

void klog_logger_init(void) {
  uart_init();
  klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
  klogf("PiPaPo booting... [qemu_arm]\n");
  klogf("UART: CMSDK UART0 @ 0x40004000\n");
  klogf("Clock: emulated (no PLL)\n");
}
