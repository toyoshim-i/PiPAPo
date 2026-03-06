/*
 * lcd.c — LCD controller driver for PicoCalc (ST7365P / ILI9488)
 *
 * Initializes the display controller via MIPI DCS and vendor-specific
 * commands, then provides basic drawing primitives.
 *
 * The PicoCalc panel is 320×320 pixels driven by a 320×480 controller;
 * we constrain the address window to the top 320 rows.
 *
 * The ST7365P is the actual silicon on the PicoCalc board (marketed as
 * ILI9488-compatible).  The vendor command unlock (0xF0 C3 / 0xF0 96)
 * enables the extended register set, which is needed for RGB565 pixel
 * format over the 4-wire SPI interface.
 *
 * Init sequence derived from the community PicoCalc MicroPython driver
 * (zenodante/PicoCalc-micropython-driver) and the official ClockworkPi
 * PicoCalc repository.
 */

#include "lcd.h"
#include "spi_lcd.h"
#include "klog.h"

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
#define CMD_INVON    0x21   /* Display Inversion On              */
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

/* ── Vendor-specific commands (ST7365P / ILI9488) ──────────────────────── */

#define CMD_PGAMCTRL 0xE0   /* Positive Gamma Control            */
#define CMD_NGAMCTRL 0xE1   /* Negative Gamma Control            */
#define CMD_PWRCTL1  0xC0   /* Power Control 1                   */
#define CMD_PWRCTL2  0xC1   /* Power Control 2                   */
#define CMD_PWRCTL3  0xC2   /* Power Control 3                   */
#define CMD_VMCTRL1  0xC5   /* VCOM Control 1                    */
#define CMD_IFMODE   0xB0   /* Interface Mode Control            */
#define CMD_FRMCTR   0xB1   /* Frame Rate Control                */
#define CMD_INVTR    0xB4   /* Display Inversion Control         */
#define CMD_DISCTRL  0xB6   /* Display Function Control          */
#define CMD_ETMOD    0xB7   /* Entry Mode Set                    */
#define CMD_CMDSET   0xF0   /* Command Set Control (ST7365P)     */

/* Helper: send command + data bytes */
static void lcd_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    spi_lcd_cmd(cmd);
    if (len > 0)
        spi_lcd_data(data, len);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void lcd_init(void)
{
    /* 1. Hardware reset */
    spi_lcd_reset();

    /* 2. Software reset */
    spi_lcd_cmd(CMD_SWRESET);
    delay_ms(120);
    if (!spi_lcd_ok()) { klog("LCD: fail @ SWRESET\n"); return; }

    /* 3. Unlock ST7365P extended command set */
    lcd_cmd(CMD_CMDSET, (const uint8_t[]){0xC3}, 1);
    lcd_cmd(CMD_CMDSET, (const uint8_t[]){0x96}, 1);
    if (!spi_lcd_ok()) { klog("LCD: fail @ vendor unlock\n"); return; }

    /* 4. Memory access control */
    lcd_cmd(CMD_MADCTL, (const uint8_t[]){MADCTL_MX | MADCTL_BGR}, 1);
    /* 5. Pixel format: 16-bit RGB565 */
    lcd_cmd(CMD_COLMOD, (const uint8_t[]){0x55}, 1);
    /* 6. Frame rate control */
    lcd_cmd(CMD_FRMCTR, (const uint8_t[]){0xA0}, 1);
    /* 7. Display inversion control */
    lcd_cmd(CMD_INVTR, (const uint8_t[]){0x00}, 1);
    /* 8. Entry mode set */
    lcd_cmd(CMD_ETMOD, (const uint8_t[]){0xC6}, 1);
    if (!spi_lcd_ok()) { klog("LCD: fail @ config\n"); return; }

    /* 9. Power control */
    lcd_cmd(CMD_PWRCTL1, (const uint8_t[]){0x80, 0x06}, 2);
    lcd_cmd(CMD_PWRCTL2, (const uint8_t[]){0x15}, 1);
    lcd_cmd(CMD_PWRCTL3, (const uint8_t[]){0xA7}, 1);
    lcd_cmd(CMD_VMCTRL1, (const uint8_t[]){0x04}, 1);
    if (!spi_lcd_ok()) { klog("LCD: fail @ power\n"); return; }

    /* 10. Vendor timing adjustment */
    lcd_cmd(0xE8, (const uint8_t[]){0x40, 0x8A, 0x00, 0x00,
                                     0x29, 0x19, 0xAA, 0x33}, 8);
    if (!spi_lcd_ok()) { klog("LCD: fail @ timing\n"); return; }

    /* 11. Positive gamma correction (14 bytes) */
    lcd_cmd(CMD_PGAMCTRL,
            (const uint8_t[]){0xF0, 0x06, 0x0F, 0x05, 0x04, 0x20, 0x37,
                              0x33, 0x4C, 0x37, 0x13, 0x14, 0x2B, 0x31}, 14);
    if (!spi_lcd_ok()) { klog("LCD: fail @ pgamma\n"); return; }

    /* 12. Negative gamma correction (14 bytes) */
    lcd_cmd(CMD_NGAMCTRL,
            (const uint8_t[]){0xF0, 0x11, 0x1B, 0x11, 0x0F, 0x0A, 0x37,
                              0x43, 0x4C, 0x37, 0x13, 0x13, 0x2C, 0x32}, 14);
    if (!spi_lcd_ok()) { klog("LCD: fail @ ngamma\n"); return; }

    /* 13. Re-lock extended command set */
    lcd_cmd(CMD_CMDSET, (const uint8_t[]){0x3C}, 1);
    lcd_cmd(CMD_CMDSET, (const uint8_t[]){0x69}, 1);

    /* 14. Display inversion on */
    spi_lcd_cmd(CMD_INVON);

    /* 15. Set full 320×320 address window */
    spi_lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    if (!spi_lcd_ok()) { klog("LCD: fail @ window\n"); return; }

    /* 16. Sleep out */
    spi_lcd_cmd(CMD_SLPOUT);
    delay_ms(120);

    /* 17. Display on */
    spi_lcd_cmd(CMD_DISPON);
    delay_ms(120);
    if (!spi_lcd_ok()) { klog("LCD: fail @ dispon\n"); return; }

    /* 18. Fill screen white (diagnostic) */
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, 0xFFFF);
    if (!spi_lcd_ok()) { klog("LCD: fail @ fill\n"); return; }
    klog("LCD: init complete, fill done\n");
}

void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   uint16_t color)
{
    spi_lcd_set_window(x, y, x + w - 1, y + h - 1);
    spi_lcd_fill(color, (uint32_t)w * h);
}
