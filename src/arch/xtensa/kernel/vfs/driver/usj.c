/*
 * usj.c — USB Serial JTAG console driver (xtensa-only).
 *
 * On M5Stack CardComputer (and most ESP32-S3 dev boards) the easy
 * console interface is the USB-OTG port wired to USB Serial JTAG; the
 * UART0 pins are not exposed.  This driver provides the kernel with
 * the usj_* console-byte API declared in usj.h, distinct from the
 * UART driver interface in kernel/vfs/driver/uart.h that other targets
 * implement on actual UART hardware.
 *
 * TX: each byte is written to the USJ TX FIFO and immediately followed
 *     by usb_serial_jtag_ll_txfifo_flush() so that single characters
 *     and prompts without a trailing newline (PS1) reach the host as
 *     soon as the kernel emits them.  Without the explicit flush, USJ
 *     accumulates bytes until the next packet boundary or newline-
 *     triggered flush in ESP-IDF's logging glue, which made non-
 *     newline-terminated output look like a hang.
 *
 * RX: the kernel idle loop fires VFS_EVENT_IDLE → tty_poll_input(),
 *     which calls the registered backend's getc/rx_avail to drain
 *     bytes into the TTY input buffer and wake any blocked readers.
 *     USJ exposes non-destructive "data available" + explicit "read"
 *     calls, so no peek buffer is needed.
 *
 * USJ HAL functions are pure register accesses (no internal `syscall 0`
 * window-spill paths), so the PS.UM=1 dance the ROM-UART driver
 * needed is not required here.
 */

#include "kernel/vfs/driver/usj.h"

#include <stdint.h>

#include "hal/usb_serial_jtag_ll.h"

/* Bound on the busy-wait for TX FIFO space.  USJ has a 64-byte TX FIFO
 * that drains on each USB IN packet (≈ once per ms when the host
 * polls), so a few-thousand-iteration cap covers worst-case temporary
 * congestion without wedging the kernel if the host is disconnected. */
#define USJ_TX_WAIT_MAX 4096

void usj_init(void) { /* ESP-IDF brings USJ up before main(). */ }

int usj_putc(char c, void (*notify)(void)) {
  (void)notify;
  uint8_t b = (uint8_t)c;
  for (int i = 0; i < USJ_TX_WAIT_MAX; i++) {
    if (usb_serial_jtag_ll_txfifo_writable()) {
      usb_serial_jtag_ll_write_txfifo(&b, 1);
      usb_serial_jtag_ll_txfifo_flush();
      return 1;
    }
  }
  /* Drop on host-disconnected timeout — keep the kernel running. */
  return 0;
}

int usj_getc(void) {
  uint8_t c;
  if (!usb_serial_jtag_ll_rxfifo_data_available()) return -1;
  if (usb_serial_jtag_ll_read_rxfifo(&c, 1) != 1) return -1;
  return (int)c;
}

int usj_rx_avail(void) {
  return usb_serial_jtag_ll_rxfifo_data_available() ? 1 : 0;
}

const tty_backend_t usj_tty_backend = {
    .putc = usj_putc,
    .flush = NULL,
    .getc = usj_getc,
    .rx_avail = usj_rx_avail,
    .get_cols = NULL,
    .get_rows = NULL,
    .set_winsize = NULL,
};
