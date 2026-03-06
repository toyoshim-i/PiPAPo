/*
 * lcd.c — LCD controller driver for PicoCalc (ST7796S / ILI9488)
 *
 * Initializes the display controller via MIPI DCS commands and provides
 * basic drawing primitives.  Both ST7796S and ILI9488 share the same
 * standard command set used here.
 *
 * The PicoCalc panel is 320×320 pixels driven by a 320×480 controller;
 * we constrain the address window to the top 320 rows.
 */

#include "lcd.h"
#include "spi_lcd.h"

/* ── Timing helper ─────────────────────────────────────────────────────── */

#define DELAY_LOOPS_PER_MS  13300u   /* ~1 ms at 133 MHz */

static void delay_ms(uint32_t ms)
{
    volatile uint32_t count = ms * DELAY_LOOPS_PER_MS;
    while (count--) ;
}

/* ── MIPI DCS commands ─────────────────────────────────────────────────── */

#define CMD_SWRESET  0x01   /* Software Reset                    */
#define CMD_SLPOUT   0x11   /* Sleep Out                         */
#define CMD_DISPON   0x29   /* Display On                        */
#define CMD_CASET    0x2A   /* Column Address Set                */
#define CMD_RASET    0x2B   /* Row Address Set                   */
#define CMD_MADCTL   0x36   /* Memory Access Control             */
#define CMD_COLMOD   0x3A   /* Pixel Format Set                  */

/* MADCTL bits */
#define MADCTL_MY    (1u << 7)   /* Row address order        */
#define MADCTL_MX    (1u << 6)   /* Column address order     */
#define MADCTL_MV    (1u << 5)   /* Row/Column exchange      */
#define MADCTL_BGR   (1u << 3)   /* BGR colour order         */

/* ── Public API ─────────────────────────────────────────────────────────── */

void lcd_init(void)
{
    /* 1. Hardware reset */
    spi_lcd_reset();

    /* 2. Software reset */
    spi_lcd_cmd(CMD_SWRESET);
    delay_ms(120);

    /* 3. Sleep out */
    spi_lcd_cmd(CMD_SLPOUT);
    delay_ms(120);

    /* 4. Pixel format: 16-bit RGB565 */
    spi_lcd_cmd(CMD_COLMOD);
    uint8_t colmod = 0x55;          /* 65K colours, 16 bits/pixel */
    spi_lcd_data(&colmod, 1);

    /* 5. Memory access control (orientation)
     * Start with normal orientation (0x00).  The correct MADCTL value
     * depends on how the panel is physically mounted; adjust after
     * hardware testing if the image is mirrored or rotated. */
    spi_lcd_cmd(CMD_MADCTL);
    uint8_t madctl = 0x00;
    spi_lcd_data(&madctl, 1);

    /* 6–7. Set full 320×320 address window */
    spi_lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);

    /* 8. Display on */
    spi_lcd_cmd(CMD_DISPON);

    /* 9. Fill screen black as visual confirmation */
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, 0x0000);
}

void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   uint16_t color)
{
    spi_lcd_set_window(x, y, x + w - 1, y + h - 1);
    spi_lcd_fill(color, (uint32_t)w * h);
}
