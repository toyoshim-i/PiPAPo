/*
 * pcxt_logger.c — VFS-side logger sink for PC/XT
 *
 * Duplicates each character to COM1 and the BIOS console so GUI runs
 * have VGA logs while headless runs can capture the same stream over
 * ttyS0.
 */

#include "kernel/vfs/driver/bios_con.h"
#include "kernel/vfs/driver/pcxt_logger.h"
#include "kernel/vfs/klog.h"
#include "kernel/vfs/driver/uart.h"

static int pcxt_logger_putc(char c, void (*notify)(void)) {
  if (!uart_putc(c, notify)) return 0;
  bios_putc(c);
  return 1;
}

static void pcxt_logger_flush(void) {}

void pcxt_logger_init(void) {
  klog_set_logger(KLOG_LOGGER_PRIMARY, pcxt_logger_putc, pcxt_logger_flush);
  klog_set_logger(KLOG_LOGGER_SECONDARY, NULL, NULL);
}

/* Override weak default — called from vfs_init() */
void klog_init_logger(void) {
  uart_init();
  pcxt_logger_init();
}
