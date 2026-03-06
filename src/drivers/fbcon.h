/*
 * fbcon.h — Framebuffer text console for PicoCalc LCD
 *
 * Character-cell text console with two display modes:
 *   Mode 0: 40×20  (8×16 font, normal)
 *   Mode 1: 80×40  (4×8  font, compact / Rogue)
 *
 * Renders text to the 320×320 LCD via the SPI1 transport layer.
 */

#ifndef PPAP_DRIVERS_FBCON_H
#define PPAP_DRIVERS_FBCON_H

#include <stdint.h>

#define FBCON_MODE_NORMAL  0   /* 40×20, 8×16 font */
#define FBCON_MODE_COMPACT 1   /* 80×40, 4×8  font */

/* Initialise console in 40×20 mode, clear screen. */
void fbcon_init(void);

/* Write one character (handles \n, \r, \b, \t, scroll). */
void fbcon_putc(char c);

/* Write a null-terminated string. */
void fbcon_puts(const char *s);

/* Redraw dirty rows to the LCD. */
void fbcon_flush(void);

/* Clear the entire screen. */
void fbcon_clear(void);

/* Switch display mode (FBCON_MODE_NORMAL or FBCON_MODE_COMPACT). */
void fbcon_set_mode(int mode);

/* Query current grid dimensions. */
int fbcon_cols(void);
int fbcon_rows(void);

/* Set the current text attribute (fg in bits [3:0], bg in bits [7:4]). */
void fbcon_set_attr(uint8_t attr);

#endif /* PPAP_DRIVERS_FBCON_H */
