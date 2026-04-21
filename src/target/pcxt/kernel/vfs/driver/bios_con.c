/*
 * bios_con.c -- direct VGA text-mode console for IBM PC
 *
 * Writes glyphs straight to 0xB8000 and programs the CRTC cursor
 * registers directly (ports 0x3D4/0x3D5). Avoids INT 10h entirely:
 * SeaBIOS's optional serial console mirrors any AH=02h/06h/0Eh call
 * to COM1, which would interleave with our own uart_putc() output and
 * produce a duplicated blank line per real log line on ttyS0.
 *
 * Cursor row/col are tracked in software.  On first use we seed them
 * from the CRTC hardware cursor, which stage1/stage2 leave at the line
 * after their last BIOS print, so kernel output continues from there.
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
  BIOS_ANSI_CSI_PRIV, /* DEC private mode: \033[? */
  BIOS_ANSI_ESC_CSET, /* after ESC-( / ) / * / + / # — swallow 1 char */
};

static uint8_t bios_attr = 0x07u; /* light grey on black */
static uint8_t bios_fg = 7u;
static uint8_t bios_bg = 0u;
static uint8_t bios_bold;
static uint8_t bios_reverse;
static uint8_t bios_ansi_state;
static uint8_t bios_param_count;
static uint8_t bios_param_seen_digit;
static uint16_t bios_params[4];
static uint8_t bios_row;
static uint8_t bios_col;
static uint8_t bios_cursor_seeded;

static void bios_sync_hw_cursor(void);

/* Seed software cursor from BDA 0040:0050 (page-0 cursor position).
 * BIOS INT 10h AH=0Eh updates this BDA word as it prints, so it's the
 * authoritative record of where stage1/stage2 left off.  The CRTC
 * hardware register may lag and isn't a reliable source. */
static void bios_seed_cursor(void) {
  if (bios_cursor_seeded) return;
  bios_cursor_seeded = 1u;
  uint16_t pos;
  __asm__ volatile (
    "push %%es\n\t"
    "mov  $0x0040, %%ax\n\t"    /* ES = 0x0040 (BDA segment) */
    "mov  %%ax, %%es\n\t"
    "mov  %%es:0x50, %0\n\t"    /* page-0 cursor: low=col, high=row */
    "pop  %%es"
    : "=r"(pos) : : "ax", "memory"
  );
  uint8_t col = (uint8_t)(pos & 0xFFu);
  uint8_t row = (uint8_t)(pos >> 8);
  if (row >= BIOS_ROWS) row = BIOS_ROWS - 1u;
  bios_row = row;
  bios_col = col;
  /* Sync our value to the CRTC hardware so the visible blink cursor
   * matches where we will start writing. */
  bios_sync_hw_cursor();
}

static void bios_update_attr(void) {
  uint8_t fg = bios_fg, bg = bios_bg;
  if (bios_reverse) { fg = bios_bg; bg = bios_fg; }
  bios_attr = (uint8_t)((bg << 4) | fg | (bios_bold ? 0x08u : 0u));
}

static void bios_reset_attr(void) {
  bios_fg = 7u;
  bios_bg = 0u;
  bios_bold = 0u;
  bios_reverse = 0u;
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
    } else if (p == 2u) {
      /* dim — not supported on VGA, treat as no-bold */
      bios_bold = 0u;
      bios_update_attr();
    } else if (p == 7u) {
      bios_reverse = 1u;
      bios_update_attr();
    } else if (p == 22u) {
      bios_bold = 0u;
      bios_update_attr();
    } else if (p == 27u) {
      bios_reverse = 0u;
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

  bios_seed_cursor();

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
      } else if (ch == '\b') {
        /* Backspace: move cursor left one column */
        if (bios_col > 0u) {
          bios_col--;
          bios_sync_hw_cursor();
        }
      } else if (ch < 0x20u || ch == 0x7Fu) {
        /* Drop other control chars */
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
      } else if (ch == '(' || ch == ')' || ch == '*' || ch == '+' ||
                 ch == '#') {
        /* Two-byte charset / line-attr designators: swallow next byte. */
        bios_ansi_state = BIOS_ANSI_ESC_CSET;
      } else if (ch == 'c') {
        /* RIS — Reset to Initial State: clear screen, reset attrs, home. */
        bios_reset_attr();
        {
          uint16_t blank =
              (uint16_t)(((uint16_t)bios_attr << 8) | (uint16_t)' ');
          for (uint16_t i = 0; i < BIOS_ROWS * BIOS_COLS; i++)
            bios_vram_poke((uint16_t)(i * 2u), blank);
        }
        bios_row = 0u;
        bios_col = 0u;
        bios_sync_hw_cursor();
        bios_ansi_reset_parser();
      } else {
        /* Unknown single-byte ESC sequence — drop it silently. */
        bios_ansi_reset_parser();
      }
      return;

    case BIOS_ANSI_ESC_CSET:
      /* Swallow the designator byte (e.g. the 'B' of ESC-(-B). */
      bios_ansi_reset_parser();
      return;

    case BIOS_ANSI_CSI:
      if (ch == '?') {
        /* DEC private mode prefix — switch to private CSI parser */
        bios_ansi_state = BIOS_ANSI_CSI_PRIV;
        return;
      }
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
      } else if (ch == 'H') {
        /* CSI row;col H — cursor position (1-based, default 1;1) */
        uint16_t row = bios_params[0];
        uint16_t col = (bios_param_count >= 1u) ? bios_params[1] : 0u;
        if (row > 0u) row--;
        if (col > 0u) col--;
        if (row >= BIOS_ROWS) row = BIOS_ROWS - 1u;
        if (col >= BIOS_COLS) col = BIOS_COLS - 1u;
        bios_row = (uint8_t)row;
        bios_col = (uint8_t)col;
        bios_sync_hw_cursor();
      } else if (ch == 'J') {
        uint16_t p = bios_params[0];
        uint16_t blank =
            (uint16_t)(((uint16_t)bios_attr << 8) | (uint16_t)' ');
        if (p == 2u) {
          /* CSI 2 J — erase entire display */
          for (uint16_t i = 0; i < BIOS_ROWS * BIOS_COLS; i++)
            bios_vram_poke((uint16_t)(i * 2u), blank);
        } else if (p == 0u) {
          /* CSI 0 J — erase from cursor to end of display */
          uint16_t start =
              (uint16_t)((uint16_t)bios_row * BIOS_COLS + bios_col);
          for (uint16_t i = start; i < BIOS_ROWS * BIOS_COLS; i++)
            bios_vram_poke((uint16_t)(i * 2u), blank);
        }
      } else if (ch == 'K') {
        /* CSI 0 K — erase from cursor to end of line */
        uint16_t p = bios_params[0];
        if (p == 0u) {
          uint16_t blank =
              (uint16_t)(((uint16_t)bios_attr << 8) | (uint16_t)' ');
          for (uint8_t c = bios_col; c < BIOS_COLS; c++) {
            uint16_t pos =
                (uint16_t)(((uint16_t)bios_row * BIOS_COLS + c) * 2u);
            bios_vram_poke(pos, blank);
          }
        }
      }
      bios_ansi_reset_parser();
      return;

    case BIOS_ANSI_CSI_PRIV:
      /* DEC private mode: \033[?Nh or \033[?Nl */
      if (ch >= '0' && ch <= '9') {
        bios_param_seen_digit = 1u;
        if ((unsigned)bios_param_count >= 4u) return;
        bios_params[bios_param_count] =
            (uint16_t)(bios_params[bios_param_count] * 10u +
                       (uint16_t)(ch - '0'));
        return;
      }
      if (ch == 'h' || ch == 'l') {
        /* ?25h = show cursor, ?25l = hide cursor */
        if (bios_params[0] == 25u) {
          if (ch == 'l') {
            /* Hide cursor: move CRTC cursor off-screen */
            outb(CRTC_INDEX, CRTC_CURSOR_HI);
            outb(CRTC_DATA, 0x07u);
            outb(CRTC_INDEX, CRTC_CURSOR_LO);
            outb(CRTC_DATA, 0xD0u);
          } else {
            /* Show cursor: restore to current position */
            bios_sync_hw_cursor();
          }
        }
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

/* ── Keyboard input (INT 16h) + tty backend ─────────────────────────────── */

static int bios_con_putc(char c, void (*notify)(void)) {
  (void)notify;
  bios_putc(c);
  return 1;
}

/* Translate BIOS scan codes (AL=0) to VT100 escape sequences.
 * Since getc returns one char at a time, we buffer the tail bytes. */
static char bios_esc_buf[3];
static unsigned char bios_esc_pos;
static unsigned char bios_esc_len;

static int bios_con_getc(void) {
  /* Drain any pending escape sequence bytes first */
  if (bios_esc_pos < bios_esc_len)
    return (unsigned char)bios_esc_buf[bios_esc_pos++];

  /* BIOS INT 16h re-enables interrupts internally while reading the
   * keyboard controller.  With kernel preemption enabled, masking IRQ 0
   * at the PIC keeps the timer pending so we don't get context-switched
   * away mid-BIOS-call and risk another process re-entering INT 16h on
   * shared BIOS state.  Same pattern as bios_blk.c's INT 13h wrapper. */
  uint8_t pic_mask = inb(PIC1_DATA);
  outb(PIC1_DATA, pic_mask | 0x01u);

  /* BIOS INT 16h AH=01h: check if keystroke available (non-blocking) */
  unsigned short flags;
  unsigned short ax;
  __asm__ volatile(
      "push %%ds\n\t"
      "push %%es\n\t"
      "mov $0x0100, %%ax\n\t"
      "int $0x16\n\t"
      "pushf\n\t"
      "pop %0\n\t"
      "mov %%ax, %1\n\t"
      "pop %%es\n\t"
      "pop %%ds"
      : "=r"(flags), "=r"(ax)
      :
      : "ax", "cc", "memory");
  if (flags & 0x0040u) {
    outb(PIC1_DATA, pic_mask);
    return -1; /* ZF set = no key */
  }

  /* BIOS INT 16h AH=00h: read the keystroke (consume it) */
  __asm__ volatile(
      "push %%ds\n\t"
      "push %%es\n\t"
      "mov $0x0000, %%ax\n\t"
      "int $0x16\n\t"
      "mov %%ax, %0\n\t"
      "pop %%es\n\t"
      "pop %%ds"
      : "=r"(ax)
      :
      : "ax", "cc", "memory");

  outb(PIC1_DATA, pic_mask);

  unsigned char al = (unsigned char)(ax & 0xFFu);
  unsigned char ah = (unsigned char)(ax >> 8);

  if (al != 0) return al; /* Normal ASCII key */

  /* Extended key: translate scan code -> VT100 ESC [ <letter> */
  char letter;
  switch (ah) {
    case 0x48: letter = 'A'; break; /* Up    */
    case 0x50: letter = 'B'; break; /* Down  */
    case 0x4D: letter = 'C'; break; /* Right */
    case 0x4B: letter = 'D'; break; /* Left  */
    case 0x47: letter = 'H'; break; /* Home  */
    case 0x4F: letter = 'F'; break; /* End   */
    case 0x53: letter = '~'; break; /* Delete (ESC [ 3 ~) */
    default: return -1; /* Ignore other extended keys */
  }

  if (ah == 0x53) {
    /* Delete: ESC [ 3 ~ */
    bios_esc_buf[0] = '[';
    bios_esc_buf[1] = '3';
    bios_esc_buf[2] = '~';
    bios_esc_len = 3;
  } else {
    bios_esc_buf[0] = '[';
    bios_esc_buf[1] = letter;
    bios_esc_len = 2;
  }
  bios_esc_pos = 0;
  return 0x1B; /* ESC — caller will get '[' and letter on next calls */
}

static int bios_con_rx_avail(void) {
  /* Buffered escape sequence bytes are immediately available */
  if (bios_esc_pos < bios_esc_len) return 1;
  /* Direct BDA read: keyboard buffer head/tail at 0x0040:0x001A/0x001C */
  unsigned short head;
  unsigned short tail;
  __asm__ volatile(
      "push %%es\n\t"
      "mov $0x0040, %%ax\n\t"
      "mov %%ax, %%es\n\t"
      "mov %%es:0x1A, %0\n\t"
      "mov %%es:0x1C, %1\n\t"
      "pop %%es"
      : "=r"(head), "=r"(tail)
      :
      : "ax", "memory");
  return head != tail ? 1 : 0;
}

const tty_backend_t bios_con_backend = {
    .putc = bios_con_putc,
    .flush = NULL,
    .getc = bios_con_getc,
    .rx_avail = bios_con_rx_avail,
    .get_cols = NULL,
    .get_rows = NULL,
    .set_winsize = NULL,
};
