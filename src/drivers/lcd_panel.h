/*
 * lcd.h — LCD controller driver for PicoCalc (ST7365P / ILI9488)
 *
 * High-level LCD initialization and drawing primitives.  Uses the SPI1
 * transport layer (spi_lcd.h) for hardware communication.
 */

#ifndef PPAP_DRIVERS_LCD_H
#define PPAP_DRIVERS_LCD_H

#include <stdint.h>

#define LCD_WIDTH   320
#define LCD_HEIGHT  320

/* Initialize the LCD controller (reset + init sequence + black fill). */
void lcd_init(void);

/* Fill a rectangle with a solid RGB565 colour. */
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   uint16_t color);

#endif /* PPAP_DRIVERS_LCD_H */
