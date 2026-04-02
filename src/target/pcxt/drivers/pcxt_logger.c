/*
 * pcxt_logger.c — Core-side logger sink for PC/XT
 *
 * Keeps early boot logging entirely inside the core segment. We
 * initialise both BIOS and UART output paths here, but log through the
 * BIOS console so QEMU's -nographic mode does not interleave duplicate
 * kernel logs from two captured sinks.
 */

#include "drivers/bios_con.h"
#include "drivers/pcxt_logger.h"
#include "drivers/uart.h"
#include "klog.h"

static int pcxt_logger_putc(char c, void (*notify)(void)) {
  (void)notify;
  bios_putc(c);
  return 1;
}

static void pcxt_logger_flush(void) {}

void pcxt_logger_init(void) {
  uart_init();
  klog_set_logger(KLOG_LOGGER_PRIMARY, pcxt_logger_putc, pcxt_logger_flush);
  klog_set_logger(KLOG_LOGGER_SECONDARY, NULL, NULL);
}
