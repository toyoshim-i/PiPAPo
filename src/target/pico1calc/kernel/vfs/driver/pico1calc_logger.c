/* pico1calc_logger.c — VFS-side UART/logger for PicoCalc */

#include "common/errno.h"
#include "kernel/common/config.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/common/pico1calc_caps.h"
#include "kernel/vfs/devfs.h"
#include "kernel/vfs/driver/fbcon.h"
#include "kernel/vfs/driver/i2c.h"
#include "kernel/vfs/driver/i2c_kbd.h"
#include "kernel/vfs/driver/lcd_panel.h"
#include "kernel/vfs/driver/spi.h"
#include "kernel/vfs/driver/spi_lcd.h"
#include "kernel/vfs/driver/spi_sd.h"
#include "kernel/vfs/driver/uart.h"
#include "kernel/vfs/driver/uart_rpico.h"
#include "kernel/vfs/klog.h"
#include "kernel/vfs/procfs.h"
#include "kernel/vfs/tty.h"

bool pico1calc_has_kbd;

/* ── LCD + keyboard TTY backend ─────────────────────────────────────────── */

/* Small ring buffer for keyboard characters.
 *
 * kbd_poll() (I2C + escape sequence state) is only ever called from the
 * idle loop on Core 0 (via fbcon_avail_wrapper).  Process-context reads
 * (fbcon_getc_wrapper) consume from this ring buffer only -- they MUST NOT
 * call kbd_poll() directly because kbd.c's internal state (seq_buf, seq_pos)
 * is not thread-safe and the two callers can run on different cores. */
#define KBD_RING_SIZE 16u /* power of 2 for mask wrap */
static volatile char kbd_ring[KBD_RING_SIZE];
static volatile uint8_t kbd_ring_head; /* written by idle loop (Core 0) */
static volatile uint8_t kbd_ring_tail; /* read by process (any core)    */

static inline int kbd_ring_empty(void) {
  return kbd_ring_head == kbd_ring_tail;
}

static inline void kbd_ring_put(char c) {
  uint8_t next = (kbd_ring_head + 1u) & (KBD_RING_SIZE - 1u);
  if (next == kbd_ring_tail) return; /* full -- drop character */
  kbd_ring[kbd_ring_head] = c;
  kbd_ring_head = next;
}

static inline int kbd_ring_get(void) {
  if (kbd_ring_empty()) return -1;
  int c = (unsigned char)kbd_ring[kbd_ring_tail];
  kbd_ring_tail = (kbd_ring_tail + 1u) & (KBD_RING_SIZE - 1u);
  return c;
}

static int fbcon_getc_wrapper(void) { return kbd_ring_get(); }

static int fbcon_avail_wrapper(void) {
  if (!kbd_ring_empty()) return 1;

  /* Drain up to 8 key events per poll cycle.  The loop MUST be bounded:
   * this runs in the idle loop on Core 0.  An unbounded loop would hang
   * if kbd_poll() keeps returning -1 due to an I2C FIFO read error while
   * kbd_poll_avail() still reports data available. */
  for (int tries = 0; tries < 8 && kbd_poll_avail(); tries++) {
    int ch = kbd_poll();
    if (ch < 0) break; /* I2C error -- stop polling this cycle */
    /* Deliver Ctrl-C immediately on tty1 so compute-bound foreground
     * tasks do not need to be blocked in read(). */
    if (ch == 0x03 && tty_signal_intr(TTY_DISPLAY)) continue;
    kbd_ring_put((char)ch);
  }
  return !kbd_ring_empty();
}

static int fbcon_get_cols(void) { return fbcon_cols(); }
static int fbcon_get_rows(void) { return fbcon_rows(); }
static void fbcon_set_winsize(int c, int r) {
  if (c > 40)
    fbcon_set_mode(FBCON_MODE_COMPACT);
  else if (r > 20)
    fbcon_set_mode(FBCON_MODE_SQUARE);
  else
    fbcon_set_mode(FBCON_MODE_NORMAL);
}

/* ── LCD backlight (STM32 I2C register 0x05) ──────────────────────────── */

#define PICO_STM32_ADDR 0x1F
#define REG_ID_BKL 0x05
#define REG_ID_BAT 0x0B
#define REG_ID_OFF 0x0E
#define REG_WRITE_MASK 0x80 /* STM32 FW uses bit 7 to flag register writes */

static int bl_i2c_get(uint8_t *val) {
  return i2c_read_reg(PICO_STM32_ADDR, REG_ID_BKL, val, 1);
}

static int bl_i2c_set(uint8_t val) {
  return i2c_write_reg(PICO_STM32_ADDR, REG_ID_BKL | REG_WRITE_MASK, &val, 1);
}

/* ── Battery and power (STM32 I2C registers 0x0B, 0x0E) ──────────────── */

static int bat_i2c_read(uint8_t *buf, int len) {
  /* STM32 returns 2 bytes: [reg_echo, percentage].
   * Percentage bits 0-6 = 0-100%, bit 7 = charging flag.
   * Extract into buf[0] = percentage byte. */
  uint8_t raw[2];
  int rc = i2c_read_reg(PICO_STM32_ADDR, REG_ID_BAT, raw, 2);
  if (rc < 0) return rc;
  buf[0] = raw[1]; /* percentage (bit 7 = charging) */
  if (len > 1) buf[1] = 0;
  return 0;
}

static int power_i2c_off(void) {
  uint8_t val = 1;
  return i2c_write_reg(PICO_STM32_ADDR, REG_ID_OFF | REG_WRITE_MASK, &val, 1);
}

/* ── TTY backend ──────────────────────────────────────────────────────── */

static const tty_backend_t fbcon_backend = {
    .putc = fbcon_putc,
    .flush = fbcon_flush_deferred,
    .getc = fbcon_getc_wrapper,
    .rx_avail = fbcon_avail_wrapper,
    .get_cols = fbcon_get_cols,
    .get_rows = fbcon_get_rows,
    .set_winsize = fbcon_set_winsize,
};

/* ── Logger init and VFS event handler ────────────────────────────────── */

void klog_init_logger(void) {
  uart_init();
  klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
  klogf("PiPaPo booting... [pico1calc]\n");
  klogf("UART: 115200 bps @ 12 MHz XOSC\n");
}

void vfs_notify(int event) {
  switch (event) {
    case VFS_EVENT_WILL_PLL_CHANGE:
      klogf("PLL: configuring...\n");
      uart_tx_drain();
      break;
    case VFS_EVENT_PLL_CHANGED:
      uart_reinit_pll();
      klogf("System clock: %u MHz\n", PPAP_SYS_HZ / 1000000u);
      spi_init(400000);
      klogf("SPI0: initialised at 400 kHz\n");
      i2c_init();
      klogf("I2C1: initialised at 10 kHz\n");
      kbd_init();
      pico1calc_has_kbd = kbd_present();
      if (!pico1calc_has_kbd)
        klogf("PicoCalc peripherals not detected "
              "(skipping LCD/fbcon)\n");
      break;
    case VFS_EVENT_LATE_INIT:
      while (uart_getc() >= 0)
        ; /* drain boot noise from RX ring */
      if (kbd_present()) {
        spi_lcd_init();
        klogf("SPI1: LCD initialised at 33 MHz\n");
        lcd_init();
        if (!spi_lcd_ok())
          klogf("LCD: *** SPI timeout during init ***\n");
        else
          klogf("LCD: ST7365P initialised (320x320 RGB565)\n");
        fbcon_init();
        klogf("FBCON: text console initialised (40x20)\n");
        klog_set_logger(KLOG_LOGGER_SECONDARY, fbcon_putc,
                        fbcon_flush_deferred);
        klogf("KLOG: output mirrored to LCD\n");
        tty_set_backend(TTY_DISPLAY, &fbcon_backend);
        klogf("TTY: backend switched to LCD+keyboard\n");
        devfs_set_backlight(bl_i2c_get, bl_i2c_set);
        bl_i2c_set(128);
        klogf("BACKLIGHT: set to 128/255\n");
        procfs_set_battery(bat_i2c_read);
        devfs_set_power(power_i2c_off);
        klogf("POWER: battery monitor and power-off registered\n");
      }
      /* TODO: SD init disabled — spi_xfer() hangs on the first SPI0
       * transfer after UF2 warm boot.  The PL022 accepts TX data but
       * never produces RX data, suggesting clk_peri or GPIO mux issue.
       * Investigate: try SPI0 loopback test, verify clk_peri, check if
       * the bootloader's boot2 reconfigures pin mux for QSPI. */
#if 0
      {
        int rc = sd_init();
        if (rc == 0)
          klogf("SD: card initialised, mmcblk0 registered\n");
        else if (rc == -(int)ENODEV)
          klogf("SD: no card detected (skipping)\n");
        else
          klogf("SD: init failed (err=%u)\n", (unsigned)(-(int)rc));
      }
#else
      klogf("SD: disabled (SPI0 hang under investigation)\n");
#endif
      break;
    case VFS_EVENT_IDLE:
      tty_poll_input();
      fbcon_poll_flush();
      break;
  }
}
