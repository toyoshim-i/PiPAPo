/*
 * spi_lcd.h — SPI1 LCD driver for PicoCalc 320×320 IPS display
 *
 * Low-level SPI1 transport layer.  Sends commands and pixel data to the
 * LCD controller (ST7365P / ILI9488) via the PL022 SPI1 peripheral.
 * Controller initialisation sequence is handled by the higher-level
 * display driver (Step 3).
 */

#ifndef PPAP_DRIVERS_SPI_LCD_H
#define PPAP_DRIVERS_SPI_LCD_H

#include <stddef.h>
#include <stdint.h>

/* Initialise SPI1 peripheral and GPIO pins for LCD communication. */
void spi_lcd_init(void);

/* Hardware reset: assert RST low 10 ms, release, wait 120 ms. */
void spi_lcd_reset(void);

/* Send a single command byte (DC=0). */
void spi_lcd_cmd(uint8_t cmd);

/* Send data bytes (DC=1). */
void spi_lcd_data(const uint8_t *buf, size_t len);

/* Send 16-bit values as big-endian byte pairs (DC=1).  Used for RGB565. */
void spi_lcd_data16(const uint16_t *buf, size_t count);

/* Streaming API: keep CS asserted across multiple data16 calls.
 * Call stream_begin before the first data16_stream, stream_end after last. */
void spi_lcd_stream_begin(void);
void spi_lcd_data16_stream(const uint16_t *buf, size_t count);
void spi_lcd_stream_end(void);

/* Set the column/row address window for subsequent pixel writes. */
void spi_lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/* Fill `count` pixels with a solid RGB565 colour (after set_window). */
void spi_lcd_fill(uint16_t color, uint32_t count);

/* Returns non-zero if the LCD is responding (no SPI timeout detected). */
int spi_lcd_ok(void);

/* Diagnostic: read SPI1 status register (SR) raw value. */
uint32_t spi_lcd_read_sr(void);

#endif /* PPAP_DRIVERS_SPI_LCD_H */
