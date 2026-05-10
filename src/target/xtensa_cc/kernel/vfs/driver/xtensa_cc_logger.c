/* xtensa_cc_logger.c — VFS-side console/logger for CardComputer
 *
 * The console transport on this target is USB Serial JTAG (UART0
 * pins are reused for I2S/IR on M5Stack CardComputer), so we wire
 * klog and tty's TTY_SERIAL backend to the usj.* driver in the
 * xtensa arch overlay rather than to kernel/vfs/driver/uart.h.
 */

#include "kernel/common/config.h"
#include "kernel/vfs/driver/usj.h"
#include "kernel/vfs/klog.h"
#include "kernel/vfs/tty.h"

void klog_init_logger(void) {
  usj_init();
  klog_set_logger(KLOG_LOGGER_PRIMARY, usj_putc, NULL);
  tty_set_backend(TTY_SERIAL, &usj_tty_backend);
  klogf("PiPAPo booting... [xtensa_cc]\n");
  klogf("System clock: %lu MHz\n", (unsigned long)(PPAP_SYS_HZ / 1000000u));
}
