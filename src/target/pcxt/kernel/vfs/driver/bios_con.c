/*
 * bios_con.c -- direct VGA text-mode console for IBM PC
 *
 * Writes glyphs straight to 0xB8000 and programs the CRTC cursor
 * registers directly (ports 0x3D4/0x3D5). Avoids INT 10h entirely:
 * SeaBIOS's optional serial console mirrors any AH=02h/06h/0Eh call
 * to COM1, which would interleave with our own uart_putc() output and
 * produce a duplicated blank line per real log line on ttyS0.
 *
 * Cursor row/col are tracked in software and seeded to (0,0); we own
 * the screen from boot, so the kernel banner starts at the top.
 */

#include "bios_con.h"

#include <stdint.h>

#include "kernel/common/ioregs.h"

#define BIOS_VRAM_SEG  0xB800u
#define BIOS_COLS      80u
#define BIOS_ROWS      25u
#define CRTC_INDEX     0x3D4u
#define CRTC_DATA      0x3D5u
#define CRTC_CURSOR_HI 0x0Eu
#define CRTC_CURSOR_LO 0x0Fu

enum bios_ansi_state {
  BIOS_ANSI_TEXT = 0,
  BIOS_ANSI_ESC,
  BIOS_ANSI_CSI,
};

static uint8_t bios_attr = 0x07u; /* light grey on black */
static uint8_t bios_fg = 7u;
static uint8_t bios_bg = 0u;
static uint8_t bios_bold;
static uint8_t bios_ansi_state;
static uint8_t bios_param_count;
static uint8_t bios_param_seen_digit;
static uint16_t bios_params[4];
static uint8_t bios_row;
static uint8_t bios_col;

static void bios_update_attr(void) {
  bios_attr = (uint8_t)((bios_bg << 4) | bios_fg | (bios_bold ? 0x08u : 0u));
}

static void bios_reset_attr(void) {
  bios_fg = 7u;
  bios_bg = 0u;
  bios_bold = 0u;
  bios_update_attr();
}

/* Write a single (attr<<8 | glyph) word to 0xB8000:offset. */
static void bios_vram_poke(uint16_t offset, uint16_t cell) {
  __asm__ volatile(
    "push %%es\n\t"
    "mov  %%bx, %%es\n\t"
    "mov  %%ax, %%es:(%%di)\n\t"
    "pop %%es"
    :
    : "a"(cell), "b"((unsigned short)BIOS_VRAM_SEG), "D"(offset)
    : "memory"
  );
}

static uint16_t bios_vram_peek(uint16_t offset) {
  uint16_t cell;
  __asm__ volatile(
    "push %%es\n\t"
    "mov  %%bx, %%es\n\t"
    "mov  %%es:(%%di), %%ax\n\t"
    "pop %%es"
    : "=a"(cell)
    : "b"((unsigned short)BIOS_VRAM_SEG), "D"(offset)
    : "memory"
  );
  return cell;
}

static void bios_sync_hw_cursor(void) {
  uint16_t pos = (uint16_t)((uint16_t)bios_row * BIOS_COLS + bios_col);
  outb(CRTC_INDEX, CRTC_CURSOR_HI);
  outb(CRTC_DATA, (uint8_t)(pos >> 8));
  outb(CRTC_INDEX, CRTC_CURSOR_LO);
  outb(CRTC_DATA, (uint8_t)(pos & 0xFFu));
}

static void bios_scroll_one(void) {
  /* Copy rows 1..24 up by one row, blank row 24. */
  uint16_t blank = (uint16_t)(((uint16_t)bios_attr << 8) | (uint16_t)' ');
  for (uint16_t i = 0; i < (BIOS_ROWS - 1u) * BIOS_COLS; i++) {
    bios_vram_poke((uint16_t)(i * 2u),
                   bios_vram_peek((uint16_t)((i + BIOS_COLS) * 2u)));
  }
  for (uint16_t i = (BIOS_ROWS - 1u) * BIOS_COLS; i < BIOS_ROWS * BIOS_COLS;
       i++) {
    bios_vram_poke((uint16_t)(i * 2u), blank);
  }
}

static void bios_newline(void) {
  bios_col = 0u;
  if (bios_row + 1u >= BIOS_ROWS) {
    bios_scroll_one();
  } else {
    bios_row++;
  }
  bios_sync_hw_cursor();
}

static void bios_carriage_return(void) {
  bios_col = 0u;
  bios_sync_hw_cursor();
}

static void bios_write_attr_char(char c) {
  uint16_t pos = (uint16_t)(((uint16_t)bios_row * BIOS_COLS + bios_col) * 2u);
  uint16_t cell = (uint16_t)(((uint16_t)bios_attr << 8) | (uint8_t)c);

  bios_vram_poke(pos, cell);

  bios_col++;
  if (bios_col >= BIOS_COLS) {
    bios_col = 0u;
    if (bios_row + 1u >= BIOS_ROWS) {
      bios_scroll_one();
    } else {
      bios_row++;
    }
  }
  bios_sync_hw_cursor();
}

static void bios_ansi_apply_sgr(void) {
  unsigned count = bios_param_count;

  if (!count && !bios_param_seen_digit) {
    bios_reset_attr();
    return;
  }
  for (unsigned i = 0; i < count; i++) {
    uint16_t p = bios_params[i];

    if (p == 0u) {
      bios_reset_attr();
    } else if (p == 1u) {
      bios_bold = 1u;
      bios_update_attr();
    } else if (p == 22u) {
      bios_bold = 0u;
      bios_update_attr();
    } else if (p >= 30u && p <= 37u) {
      bios_fg = (uint8_t)(p - 30u);
      bios_update_attr();
    } else if (p == 39u) {
      bios_fg = 7u;
      bios_update_attr();
    } else if (p >= 40u && p <= 47u) {
      bios_bg = (uint8_t)(p - 40u);
      bios_update_attr();
    } else if (p == 49u) {
      bios_bg = 0u;
      bios_update_attr();
    }
  }
}

static void bios_ansi_reset_parser(void) {
  bios_ansi_state = BIOS_ANSI_TEXT;
  bios_param_count = 0u;
  bios_param_seen_digit = 0u;
}

void bios_putc(char c)
{
  uint8_t ch = (uint8_t)c;

  switch (bios_ansi_state) {
    case BIOS_ANSI_TEXT:
      if (ch == 0x1Bu) {
        bios_ansi_state = BIOS_ANSI_ESC;
        return;
      }
      if (ch == '\n') {
        bios_newline();
      } else if (ch == '\r') {
        bios_carriage_return();
      } else if (ch < 0x20u || ch == 0x7Fu) {
        /* Drop other control chars rather than routing through INT 10h
         * AH=0Eh, which SeaBIOS would mirror onto the serial console. */
      } else {
        bios_write_attr_char(c);
      }
      return;

    case BIOS_ANSI_ESC:
      if (ch == '[') {
        bios_ansi_state = BIOS_ANSI_CSI;
        bios_param_count = 0u;
        bios_param_seen_digit = 0u;
        bios_params[0] = 0u;
      } else {
        bios_ansi_reset_parser();
      }
      return;

    case BIOS_ANSI_CSI:
      if (ch >= '0' && ch <= '9') {
        bios_param_seen_digit = 1u;
        if ((unsigned)bios_param_count >= 4u) return;
        bios_params[bios_param_count] =
            (uint16_t)(bios_params[bios_param_count] * 10u + (uint16_t)(ch - '0'));
        return;
      }
      if (ch == ';') {
        if ((unsigned)bios_param_count < 3u) {
          bios_param_count++;
          bios_params[bios_param_count] = 0u;
          bios_param_seen_digit = 0u;
        }
        return;
      }
      if (ch == 'm') {
        if (bios_param_seen_digit || (unsigned)bios_param_count > 0u)
          bios_param_count++;
        bios_ansi_apply_sgr();
      }
      bios_ansi_reset_parser();
      return;
  }
}

void bios_puts(const char *s)
{
  bios_reset_attr();
  bios_ansi_reset_parser();
  while (*s)
    bios_putc(*s++);
}
