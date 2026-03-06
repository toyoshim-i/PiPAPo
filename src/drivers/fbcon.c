/*
 * fbcon.c — Framebuffer text console for PicoCalc LCD
 *
 * Character-cell text console with two display modes:
 *   Mode 0: 40×20  (8×16 font, normal)
 *   Mode 1: 80×40  (4×8  font, compact / Rogue)
 *
 * Renders text to the 320×320 LCD via scanline-based streaming
 * to avoid large pixel buffers (~640 B stack per scanline).
 */

#include "drivers/fbcon.h"
#include "drivers/font.h"
#include "drivers/spi_lcd.h"
#include "drivers/lcd.h"
#include <stdint.h>
#include <string.h>

/* ---------- constants ---------- */

#define FBCON_MAX_COLS  80
#define FBCON_MAX_ROWS  40

/* ANSI 16-color palette in RGB565 (standard 8 + 8 bright) */
static const uint16_t ansi_palette[16] = {
    0x0000,  /* 0  black       */
    0x8000,  /* 1  red         */
    0x0400,  /* 2  green       */
    0x8400,  /* 3  yellow      */
    0x0010,  /* 4  blue        */
    0x8010,  /* 5  magenta     */
    0x0410,  /* 6  cyan        */
    0xC618,  /* 7  white (grey)*/
    0x8410,  /* 8  bright black (dark grey)  */
    0xF800,  /* 9  bright red                */
    0x07E0,  /* 10 bright green              */
    0xFFE0,  /* 11 bright yellow             */
    0x001F,  /* 12 bright blue               */
    0xF81F,  /* 13 bright magenta            */
    0x07FF,  /* 14 bright cyan               */
    0xFFFF,  /* 15 bright white              */
};

/* ---------- state ---------- */

/* Place large buffers in IOBUF region (24KB) to avoid RAM_KERNEL overflow */
static uint8_t cell_char[FBCON_MAX_ROWS][FBCON_MAX_COLS]
    __attribute__((section(".iobuf")));                     /* 3200 B */
static uint8_t cell_attr[FBCON_MAX_ROWS][FBCON_MAX_COLS]
    __attribute__((section(".iobuf")));                     /* 3200 B */
static uint8_t dirty[FBCON_MAX_ROWS];                      /* 40 B   */

static int cols, rows;
static int cursor_x, cursor_y;
static int font_w, font_h;
static int font_stride;          /* bytes per glyph = font_h */
static const uint8_t *font_data; /* pointer to font8x16 or font4x8 */
static uint8_t cur_attr;         /* fg [3:0], bg [7:4] */

/* ---------- internal helpers ---------- */

static void scroll_up(void)
{
    memmove(cell_char[0], cell_char[1],
            (uint32_t)(rows - 1) * FBCON_MAX_COLS);
    memmove(cell_attr[0], cell_attr[1],
            (uint32_t)(rows - 1) * FBCON_MAX_COLS);
    memset(cell_char[rows - 1], ' ', (uint32_t)cols);
    memset(cell_attr[rows - 1], cur_attr, (uint32_t)cols);
    for (int r = 0; r < rows; r++)
        dirty[r] = 1;
}

/* Render one scanline of a row into buf[0..cols*font_w-1]. */
static void render_scanline(int row, int sy, uint16_t *buf)
{
    int width = cols * font_w;
    for (int c = 0; c < cols; c++) {
        uint8_t ch   = cell_char[row][c];
        uint8_t attr = cell_attr[row][c];
        uint16_t fg  = ansi_palette[attr & 0x0F];
        uint16_t bg  = ansi_palette[(attr >> 4) & 0x0F];
        uint8_t bits = font_data[ch * font_stride + sy];
        int base = c * font_w;
        for (int px = 0; px < font_w; px++) {
            buf[base + px] = (bits & (0x80 >> px)) ? fg : bg;
        }
    }
    /* Clear any remaining pixels if cols*font_w < LCD_WIDTH */
    for (int x = width; x < LCD_WIDTH; x++)
        buf[x] = 0x0000;
}

/* ---------- public API ---------- */

void fbcon_init(void)
{
    fbcon_set_mode(FBCON_MODE_NORMAL);
}

void fbcon_putc(char c)
{
    uint8_t ch = (uint8_t)c;

    if (ch >= 0x20 && ch <= 0x7E) {
        /* Printable character */
        cell_char[cursor_y][cursor_x] = ch;
        cell_attr[cursor_y][cursor_x] = cur_attr;
        dirty[cursor_y] = 1;
        cursor_x++;
        if (cursor_x >= cols) {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y >= rows) {
                scroll_up();
                cursor_y = rows - 1;
            }
        }
    } else if (ch == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= rows) {
            scroll_up();
            cursor_y = rows - 1;
        }
    } else if (ch == '\r') {
        cursor_x = 0;
    } else if (ch == '\b') {
        if (cursor_x > 0)
            cursor_x--;
    } else if (ch == '\t') {
        int next = (cursor_x + 8) & ~7;
        if (next > cols)
            next = cols;
        cursor_x = next;
        if (cursor_x >= cols) {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y >= rows) {
                scroll_up();
                cursor_y = rows - 1;
            }
        }
    }
}

void fbcon_puts(const char *s)
{
    while (*s)
        fbcon_putc(*s++);
}

void fbcon_flush(void)
{
    /* Stack buffer for one scanline — 320 pixels × 2 bytes = 640 B */
    uint16_t line[LCD_WIDTH];

    for (int r = 0; r < rows; r++) {
        if (!dirty[r])
            continue;
        for (int sy = 0; sy < font_h; sy++) {
            int y = r * font_h + sy;
            spi_lcd_set_window(0, (uint16_t)y,
                               (uint16_t)(LCD_WIDTH - 1), (uint16_t)y);
            render_scanline(r, sy, line);
            spi_lcd_data16(line, LCD_WIDTH);
        }
        dirty[r] = 0;
    }
}

void fbcon_clear(void)
{
    memset(cell_char, ' ', sizeof(cell_char));
    memset(cell_attr, cur_attr, sizeof(cell_attr));
    cursor_x = 0;
    cursor_y = 0;
    for (int r = 0; r < rows; r++)
        dirty[r] = 1;
}

void fbcon_set_mode(int mode)
{
    if (mode == FBCON_MODE_COMPACT) {
        cols = 80;
        rows = 40;
        font_w = 4;
        font_h = 8;
        font_stride = 8;
        font_data = &font4x8[0][0];
    } else {
        cols = 40;
        rows = 20;
        font_w = 8;
        font_h = 16;
        font_stride = 16;
        font_data = &font8x16[0][0];
    }
    cur_attr = 0x07;  /* white on black */
    fbcon_clear();
    fbcon_flush();
}

int fbcon_cols(void) { return cols; }
int fbcon_rows(void) { return rows; }

void fbcon_set_attr(uint8_t attr) { cur_attr = attr; }
