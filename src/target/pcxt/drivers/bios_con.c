/*
 * bios_con.c -- BIOS INT 10h text output for IBM PC
 *
 * Printable characters are written directly to VGA text memory so tty1
 * can translate a small subset of ANSI SGR escapes into on-screen
 * colors without relying on BIOS AH=09h quirks. Headless users should
 * use ttyS0, which already preserves raw ANSI.
 */

#include "bios_con.h"

#include <stdint.h>

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

static void bios_update_attr(void) {
  bios_attr = (uint8_t)((bios_bg << 4) | bios_fg | (bios_bold ? 0x08u : 0u));
}

static void bios_reset_attr(void) {
  bios_fg = 7u;
  bios_bg = 0u;
  bios_bold = 0u;
  bios_update_attr();
}

static void bios_teletype(char c) {
  __asm__ volatile(
    "push %%ds\n\t"
    "push %%es\n\t"
    "int $0x10\n\t"
    "pop %%es\n\t"
    "pop %%ds"
    :
    : "a"((unsigned short)(0x0E00u | (unsigned char)c)),
      "b"((unsigned short)0x0007u)
    : "cc", "memory"
  );
}

static void bios_write_attr_char(char c) {
  uint16_t cursor;
  uint16_t pos;
  uint16_t cell;
  uint8_t glyph = (uint8_t)c;
  uint8_t row;
  uint8_t col;

  __asm__ volatile(
    "push %%ds\n\t"
    "push %%es\n\t"
    "int $0x10\n\t"
    "pop %%es\n\t"
    "pop %%ds"
    : "=d"(cursor)
    : "a"((unsigned short)0x0300u),
      "b"((unsigned short)0x0000u)
    : "cx", "cc", "memory"
  );

  col = (uint8_t)(cursor & 0x00FFu);
  row = (uint8_t)(cursor >> 8);
  pos = (uint16_t)(((uint16_t)row * 80u + (uint16_t)col) * 2u);
  cell = (uint16_t)(((uint16_t)bios_attr << 8) | glyph);

  __asm__ volatile(
    "push %%es\n\t"
    "mov  $0xB800, %%bx\n\t"
    "mov  %%bx, %%es\n\t"
    "mov  %%ax, %%es:(%%di)\n\t"
    "pop %%es"
    :
    : "a"(cell), "D"(pos)
    : "bx", "memory"
  );

  col++;
  if (col >= 80u) {
    col = 0u;
    row++;
    if (row >= 25u) {
      row = 24u;
      __asm__ volatile(
        "push %%ds\n\t"
        "push %%es\n\t"
        "int $0x10\n\t"
        "pop %%es\n\t"
        "pop %%ds"
        :
        : "a"((unsigned short)0x0601u),
          "b"((unsigned short)(0x0000u | bios_attr)),
          "c"((unsigned short)0x0000u),
          "d"((unsigned short)0x184Fu)
        : "cc", "memory"
      );
    }
  }

  __asm__ volatile(
    "push %%ds\n\t"
    "push %%es\n\t"
    "int $0x10\n\t"
    "pop %%es\n\t"
    "pop %%ds"
    :
    : "a"((unsigned short)0x0200u),
      "b"((unsigned short)0x0000u),
      "d"((unsigned short)(((unsigned short)row << 8) | col))
    : "cc", "memory"
  );
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
      if (ch < 0x20u || ch == 0x7Fu) {
        bios_teletype(c);
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
