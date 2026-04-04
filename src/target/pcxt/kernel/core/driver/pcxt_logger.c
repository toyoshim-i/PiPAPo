/*
 * pcxt_logger.c — Core-side logger sink for PC/XT
 *
 * Keeps early boot logging entirely inside the core segment. The logger
 * duplicates each character to COM1 and the BIOS console so GUI runs
 * have VGA logs while headless runs can capture the same stream over
 * ttyS0.
 */

#include "kernel/core/driver/bios_con.h"
#include "kernel/core/driver/pcxt_logger.h"
#include "kernel/core/driver/uart.h"
#include "kernel/common/mod/mod_vfs.h"

static int pcxt_logger_putc(char c, void (*notify)(void)) {
  if (!uart_putc(c, notify)) return 0;
  bios_putc(c);
  return 1;
}

static void pcxt_logger_flush(void) {}

void pcxt_logger_init(void) {
  /* uart_init() is called separately before seg_init_modules() so that
   * hardware is ready before mod_vfs far pointers are patched. */
  mod_vfs.klog_set_logger(KLOG_LOGGER_PRIMARY, pcxt_logger_putc, pcxt_logger_flush);
  mod_vfs.klog_set_logger(KLOG_LOGGER_SECONDARY, NULL, NULL);
}
