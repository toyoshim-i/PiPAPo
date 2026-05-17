/* xtensa_cc_logger.c — VFS-side console/logger for CardComputer
 *
 * Primary console transport: USB Serial JTAG (UART0 pins are reused for
 * I2S/IR on M5Stack CardComputer), wired via the usj.* driver in the
 * xtensa arch overlay.
 *
 * Secondary console: ST7789V2 LCD via fbcon, brought up at
 * VFS_EVENT_LATE_INIT — the order matches pico1calc's pattern
 * (spi → lcd → fbcon → klog secondary → tty backend → kbd → backlight on).
 * Backlight (GPIO38) is asserted last so the post-reset garbage never
 * reaches the user.  The TTY backend's getc / rx_avail are served by a
 * 16-byte ring buffer that the idle-loop drain wrapper feeds from
 * cc_kbd's matrix scanner.
 *
 * Idle hook: USJ RX needs tty_poll_input() and fbcon needs
 * fbcon_poll_flush() to push deferred dirty rows down the SPI bus.
 */

#include <driver/gpio.h>
#include <stdint.h>

#include "kernel/common/config.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/common/xtensa_cc.h"
#include "kernel/core/mm/mem_helper.h"
#include "kernel/vfs/driver/cc_kbd.h"
#include "kernel/vfs/driver/fbcon.h"
#include "kernel/vfs/driver/lcd_panel.h"
#include "kernel/vfs/driver/spi_lcd.h"
#include "kernel/vfs/driver/usj.h"
#include "kernel/vfs/klog.h"
#include "kernel/vfs/tty.h"

/* ── Keyboard input ring buffer ──────────────────────────────────────────
 *
 * Same pattern as pico1calc_logger.c:25-73.  cc_kbd's poll path holds
 * non-thread-safe state (the escape-sequence cursor); we keep all calls
 * to it on the idle loop via fbcon_avail_wrapper, and serve process-
 * context reads (fbcon_getc_wrapper) from this ring buffer only.
 *
 * KBD_RING_SIZE is a power of two so the index advance can use a mask
 * instead of a modulo.  Volatile head/tail give cross-context ordering
 * without needing the spinlock infrastructure (single core, no SMP). */

#define KBD_RING_SIZE 16u
static volatile char kbd_ring[KBD_RING_SIZE];
static volatile uint8_t kbd_ring_head; /* written by idle drain */
static volatile uint8_t kbd_ring_tail; /* read by process context */

static inline int kbd_ring_empty(void) {
  return kbd_ring_head == kbd_ring_tail;
}

static inline void kbd_ring_put(char c) {
  uint8_t next = (kbd_ring_head + 1u) & (KBD_RING_SIZE - 1u);
  if (next == kbd_ring_tail) return; /* full — drop character */
  kbd_ring[kbd_ring_head] = c;
  kbd_ring_head = next;
}

static inline int kbd_ring_get(void) {
  if (kbd_ring_empty()) return -1;
  int c = (unsigned char)kbd_ring[kbd_ring_tail];
  kbd_ring_tail = (kbd_ring_tail + 1u) & (KBD_RING_SIZE - 1u);
  return c;
}

/* ── TTY backend wrappers ─────────────────────────────────────────────────
 *
 * fbcon_cols() / fbcon_rows() return int; the tty_backend_t signatures
 * are int (*)(void), so the API shapes match exactly — but we wrap to
 * keep the backend struct field initialisers cleanly typed and to
 * leave space for future per-mode bookkeeping. */

static int fbcon_get_cols_wrapper(void) { return fbcon_cols(); }
static int fbcon_get_rows_wrapper(void) { return fbcon_rows(); }

static int fbcon_getc_wrapper(void) { return kbd_ring_get(); }

static int fbcon_avail_wrapper(void) {
  if (!kbd_ring_empty()) return 1;
  /* Drain up to 8 bytes per poll cycle.  Bound is mandatory: this runs
   * on the idle loop, an unbounded loop would freeze user-space if the
   * scan path ever returned successive bytes without progress. */
  for (int tries = 0; tries < 8 && kbd_poll_avail(); tries++) {
    int ch = kbd_poll();
    if (ch < 0) break;
    /* Hoist Ctrl-C to the line discipline so a foreground task that
     * is not blocked in read() still sees SIGINT — same rationale as
     * pico1calc's tty_signal_intr fast path. */
    if (ch == 0x03 && tty_signal_intr(TTY_DISPLAY)) continue;
    kbd_ring_put((char)ch);
  }
  return !kbd_ring_empty();
}

static const tty_backend_t fbcon_backend = {
    .putc = fbcon_putc,
    .flush = fbcon_flush_deferred,
    .getc = fbcon_getc_wrapper,
    .rx_avail = fbcon_avail_wrapper,
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
      /* 4×8 font: 240/4 × 135/8 = 60 × 16.  Twice the column count of
       * MODE_SQUARE (8×8 → 30 × 16) at the cost of half-width
       * characters — readable on the panel's ~240 ppi horizontal and
       * a much better fit for typical shell output (ls, error lines,
       * banners) that gets clipped at 30 columns. */
      fbcon_set_mode(FBCON_MODE_COMPACT);
      klog_set_logger(KLOG_LOGGER_SECONDARY, fbcon_putc, fbcon_flush);
      tty_set_backend(TTY_DISPLAY, &fbcon_backend);
      gpio_set_level(DISPLAY_BL_PIN, 1); /* backlight on, suppress garbage */
      klogf("LCD: console mirrored to display, backlight on\n");

      /* Keyboard.  Safe to bring up after tty_set_backend even though
       * fbcon_backend.getc/.rx_avail already point at the kbd drain
       * wrappers — no process can read /dev/tty1 yet (init hasn't
       * started).  Keeping LCD and KBD as separate setup blocks makes
       * the boot log read in the obvious order. */
      kbd_init();
      klogf("KBD: matrix scan enabled (60x16 console accepts input)\n");

      /* Re-emit page-pool / arena sizing now that the serial monitor has
       * attached.  mm_init's banner fired before reset → flash → monitor
       * boot, so the test-harness capture (xtensa_cc_test_monitor.py)
       * misses it; this call surfaces the same data inside the captured
       * window so size and ram_text-alias status are visible at-test. */
      mem_helper_log_state();
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
