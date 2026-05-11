/*
 * lcd_st7789.c — Sitronix ST7789V2 LCD controller driver
 *
 * Implements the generic lcd_panel.h contract (lcd_init / lcd_fill_rect)
 * for ST7789V2-driven IPS panels.  Used today by the M5Stack
 * CardComputer's 240×135 module; sized to work on any ST7789V2
 * variant by reading the visible window dimensions from the
 * target-provided lcd_geom.h.
 *
 * The ST7789V2 silicon backs a 240×320 RAM frame.  Most physical
 * panels expose a smaller visible window at a fixed pixel offset
 * inside that frame; the transport layer (e.g. spi_lcd_xtensa_cc.c)
 * applies the LCD_COL_OFFSET / LCD_ROW_OFFSET from lcd_geom.h to
 * every CASET / RASET so callers here and in fbcon work in a
 * (0,0)-origin visible coordinate space.  Picking a different
 * MADCTL orientation requires updating the per-target offsets to
 * match — see the target's lcd_geom.h.
 */

#include "kernel/common/mod/mod_vfs.h"
#include "kernel/vfs/driver/lcd_panel.h"
#include "kernel/vfs/driver/spi_lcd.h"

/* ── Timing helper ─────────────────────────────────────────────────────── */

#define DELAY_LOOPS_PER_MS 240000u /* ~1 ms; conservative on 240 MHz cores */

static void delay_ms(uint32_t ms) {
  volatile uint32_t count = ms * DELAY_LOOPS_PER_MS;
  while (count--);
}

/* ── MIPI DCS commands ─────────────────────────────────────────────────── */

#define CMD_SWRESET 0x01 /* Software Reset                    */
#define CMD_SLPOUT 0x11  /* Sleep Out                         */
#define CMD_NORON 0x13   /* Normal Display Mode On            */
#define CMD_INVON 0x21   /* Display Inversion On (IPS panels) */
#define CMD_DISPON 0x29  /* Display On                        */
#define CMD_CASET 0x2A   /* Column Address Set                */
#define CMD_RASET 0x2B   /* Row Address Set                   */
#define CMD_RAMWR 0x2C   /* Memory Write                      */
#define CMD_MADCTL 0x36  /* Memory Access Control             */
#define CMD_COLMOD 0x3A  /* Pixel Format Set                  */

/* MADCTL bits */
#define MADCTL_MY (1u << 7)  /* Row address order        */
#define MADCTL_MX (1u << 6)  /* Column address order     */
#define MADCTL_MV (1u << 5)  /* Row/Column exchange      */
#define MADCTL_BGR (1u << 3) /* BGR colour order (0 = RGB) */

#define MADCTL_LANDSCAPE (MADCTL_MV | MADCTL_MX)

/* Helper: send command + optional data payload. */
static void lcd_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
  spi_lcd_cmd(cmd);
  if (len > 0) spi_lcd_data(data, len);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void lcd_init(void) {
  /* 1. Hardware reset (handled by transport). */
  spi_lcd_reset();

  /* 2. Software reset for good measure — the controller's power-on
   *    state can be unpredictable after a warm boot. */
  spi_lcd_cmd(CMD_SWRESET);
  delay_ms(150);
  if (!spi_lcd_ok()) {
    mod_vfs.klogf("LCD: fail @ SWRESET\n");
    return;
  }

  /* 3. Sleep out — required before any pixel commands. */
  spi_lcd_cmd(CMD_SLPOUT);
  delay_ms(120);

  /* 4. Pixel format: 16-bit RGB565. */
  lcd_cmd(CMD_COLMOD, (const uint8_t[]){0x55}, 1);

  /* 5. Memory access control: landscape orientation, RGB order. */
  lcd_cmd(CMD_MADCTL, (const uint8_t[]){MADCTL_LANDSCAPE}, 1);

  /* 6. IPS inversion ON — required for correct colours on every
   *    ST7789V2 IPS module the author has seen.  TN variants would
   *    use INVOFF (0x20); none ship on CardComputer. */
  spi_lcd_cmd(CMD_INVON);

  /* 7. Normal display mode (leave partial / idle states). */
  spi_lcd_cmd(CMD_NORON);

  /* 8. Default address window: the entire visible LCD area. */
  spi_lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
  if (!spi_lcd_ok()) {
    mod_vfs.klogf("LCD: fail @ window\n");
    return;
  }

  /* 9. Clear the framebuffer before the panel comes on — eliminates
   *    the post-reset garbage flash. */
  lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, 0x0000);
  if (!spi_lcd_ok()) {
    mod_vfs.klogf("LCD: fail @ clear\n");
    return;
  }

  /* 10. Display on. */
  spi_lcd_cmd(CMD_DISPON);
  delay_ms(120);
  if (!spi_lcd_ok()) {
    mod_vfs.klogf("LCD: fail @ dispon\n");
    return;
  }

  mod_vfs.klogf("LCD: ST7789V2 initialised (%ux%u RGB565)\n",
                (unsigned)LCD_WIDTH, (unsigned)LCD_HEIGHT);
}

void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   uint16_t color) {
  spi_lcd_set_window(x, y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
  spi_lcd_fill(color, (uint32_t)w * h);
}
