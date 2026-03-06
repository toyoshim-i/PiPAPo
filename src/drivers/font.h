/*
 * font.h — Bitmap font declarations for PicoCalc text console
 *
 * Two fixed-width bitmap fonts generated at build time from BDF sources:
 *   - font8x16: 8×16 pixels, 40×20 character grid (normal mode)
 *   - font4x8:  4×8  pixels, 80×40 character grid (compact mode)
 *
 * Each glyph is stored as H bytes, one byte per row, MSB = leftmost pixel.
 * For 4-pixel-wide glyphs, only the top 4 bits of each byte are used.
 */

#ifndef PPAP_DRIVERS_FONT_H
#define PPAP_DRIVERS_FONT_H

#include <stdint.h>

extern const uint8_t font8x16[256][16];  /* 4 KB in .rodata (flash) */
extern const uint8_t font4x8[256][8];    /* 2 KB in .rodata (flash) */

#endif /* PPAP_DRIVERS_FONT_H */
