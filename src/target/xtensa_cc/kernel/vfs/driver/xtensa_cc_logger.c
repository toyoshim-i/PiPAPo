/* xtensa_cc_logger.c — VFS-side console/logger for CardComputer
 *
 * Primary console transport: USB Serial JTAG (UART0 pins are reused for
 * I2S/IR on M5Stack CardComputer), wired via the usj.* driver in the
 * xtensa arch overlay.
 *
 * Secondary console: ST7789V2 LCD via fbcon, brought up at
 * VFS_EVENT_LATE_INIT — the order matches pico1calc's pattern
 * (spi → lcd → fbcon → klog secondary → tty backend → backlight on).
 * The display console becomes the secondary klog sink and the
 * TTY_DISPLAY backend; input getc/rx_avail land with the keyboard in
 * CC-5.  Backlight (GPIO38) is asserted last so the post-reset garbage
 * never reaches the user.
 *
 * Idle hook: USJ RX needs tty_poll_input() and fbcon needs
 * fbcon_poll_flush() to push deferred dirty rows down the SPI bus.
 */

#include <driver/gpio.h>

#include "kernel/common/config.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/common/xtensa_cc.h"
#include "kernel/vfs/driver/fbcon.h"
#include "kernel/vfs/driver/lcd_panel.h"
#include "kernel/vfs/driver/spi_lcd.h"
#include "kernel/vfs/driver/usj.h"
#include "kernel/vfs/klog.h"
#include "kernel/vfs/tty.h"

/* ── TTY backend wrappers ─────────────────────────────────────────────────
 *
 * fbcon_cols() / fbcon_rows() return int; the tty_backend_t signatures
 * are int (*)(void), so the API shapes match exactly — but we wrap to
 * keep the backend struct field initialisers cleanly typed and to
 * leave space for future per-mode bookkeeping. */

static int fbcon_get_cols_wrapper(void) { return fbcon_cols(); }
static int fbcon_get_rows_wrapper(void) { return fbcon_rows(); }

static const tty_backend_t fbcon_backend = {
    .putc = fbcon_putc,
    .flush = fbcon_flush_deferred,
    /* .getc / .rx_avail — CC-5 wires the keyboard here */
    .get_cols = fbcon_get_cols_wrapper,
    .get_rows = fbcon_get_rows_wrapper,
    /* .set_winsize — xtensa_cc uses a single mode (SQUARE → 30×16) */
};

void klog_init_logger(void) {
  /* Idempotent: target_early_init dispatches VFS_EVENT_MODULE_READY so the
   * USJ logger is online before mm_init's "MM:" banner prints, and
   * vfs_init() later calls this again unconditionally.  Re-emitting the
   * boot banner from the second call would be noise. */
  static int initialized;
  if (initialized) return;
  initialized = 1;
  usj_init();
  klog_set_logger(KLOG_LOGGER_PRIMARY, usj_putc, NULL);
  tty_set_backend(TTY_SERIAL, &usj_tty_backend);
  klogf("PiPAPo booting... [xtensa_cc]\n");
  klogf("System clock: %lu MHz\n", (unsigned long)(PPAP_SYS_HZ / 1000000u));
}

void vfs_notify(int event) {
  switch (event) {
    case VFS_EVENT_MODULE_READY:
      /* target_early_init dispatches this before mm_init so the "MM:"
       * memory-map banner has a logger to write to.  klog_init_logger
       * is idempotent — vfs_init() will call it again later. */
      klog_init_logger();
      break;

    case VFS_EVENT_LATE_INIT:
      spi_lcd_init();
      if (!spi_lcd_ok()) {
        klogf("LCD: SPI transport init failed — display disabled\n");
        break;
      }
      lcd_init();
      if (!spi_lcd_ok()) {
        klogf("LCD: panel init failed — display disabled\n");
        break;
      }
      fbcon_init();
      /* 240/8 × 135/8 = 30 × 16 — the only sensible grid for this panel. */
      fbcon_set_mode(FBCON_MODE_SQUARE);
      klog_set_logger(KLOG_LOGGER_SECONDARY, fbcon_putc, fbcon_flush);
      tty_set_backend(TTY_DISPLAY, &fbcon_backend);
      gpio_set_level(DISPLAY_BL_PIN, 1); /* backlight on, suppress garbage */
      klogf("LCD: console mirrored to display, backlight on\n");
      break;

    case VFS_EVENT_IDLE:
      /* The weak default in vfs.c would call tty_poll_input(); since we
       * override vfs_notify here, replicate that call and add the fbcon
       * deferred-flush pump. */
      tty_poll_input();
      fbcon_poll_flush();
      break;
  }
}
