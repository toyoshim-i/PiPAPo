/* qemu_m68k_logger.c — VFS-side UART/logger for QEMU m68k */

#include "kernel/common/mod/mod_vfs.h"
#include "kernel/vfs/driver/uart.h"
#include "kernel/vfs/klog.h"

extern void sched_set_input_poll(int (*fn)(void), int tty_idx);

void klog_init_logger(void) {
  uart_init();
  klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
  klogf("PiPaPo booting... [qemu_m68k]\n");
  klogf("UART: Goldfish TTY @ 0xFF008000\n");
  klogf("Clock: emulated (no PLL)\n");
}

void vfs_notify(int event) {
  if (event == VFS_EVENT_LATE_INIT)
    sched_set_input_poll(uart_rx_avail, 0 /* TTY_SERIAL */);
}
