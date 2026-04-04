/*
 * fbcon.c — Framebuffer text console for PicoCalc LCD
 *
 * Character-cell text console with two display modes:
 *   Mode 0: 40×20  (8×16 font, normal)
 *   Mode 1: 80×40  (4×8  font, compact / Rogue)
 *
 * Renders text to the 320×320 LCD via scanline-based streaming
 * to avoid large pixel buffers (~640 B stack per scanline).
 *
 * Includes a VT100/ANSI CSI escape sequence parser for cursor
 * movement, screen/line erase, SGR colors, and scroll regions.
 */

#include "fbcon.h"

#include <stdint.h>
#include <string.h>

#include "font.h"
#include "lcd_panel.h"
#include "spi_lcd.h"

/* ---------- constants ---------- */

#define FBCON_MAX_COLS 80
#define FBCON_MAX_ROWS 40
#define VT_MAX_PARAMS 8

/* ANSI 16-color palette in RGB565 (standard 8 + 8 bright) */
static const uint16_t ansi_palette[16] = {
    0x0000, /* 0  black       */
    0x8000, /* 1  red         */
    0x0400, /* 2  green       */
    0x8400, /* 3  yellow      */
    0x0010, /* 4  blue        */
    0x8010, /* 5  magenta     */
    0x0410, /* 6  cyan        */
    0xC618, /* 7  white (grey)*/
    0x8410, /* 8  bright black (dark grey)  */
    0xF800, /* 9  bright red                */
    0x07E0, /* 10 bright green              */
    0xFFE0, /* 11 bright yellow             */
    0x001F, /* 12 bright blue               */
    0xF81F, /* 13 bright magenta            */
    0x07FF, /* 14 bright cyan               */
    0xFFFF, /* 15 bright white              */
};

/* ---------- state ---------- */

/* Place large buffers in IOBUF region (24KB) to avoid RAM_KERNEL overflow */
static uint8_t cell_char[FBCON_MAX_ROWS][FBCON_MAX_COLS]
    __attribute__((section(".iobuf"))); /* 3200 B */
static uint8_t cell_attr[FBCON_MAX_ROWS][FBCON_MAX_COLS]
    __attribute__((section(".iobuf"))); /* 3200 B */
static uint8_t dirty[FBCON_MAX_ROWS];   /* 40 B   */

static int cols, rows;
static int cursor_x, cursor_y;
static int font_w, font_h;
static int font_stride;          /* bytes per glyph = font_h */
static const uint8_t *font_data; /* pointer to font8x16 or font4x8 */
static uint8_t cur_attr;         /* fg [3:0], bg [7:4] */
static int bold;                 /* bold flag (maps fg to bright variant) */
static int reverse;              /* reverse video flag (swap fg/bg) */

/* Save/restore cursor (ESC 7 / ESC 8) */
static int saved_x, saved_y;
static uint8_t saved_attr;
static int saved_bold, saved_reverse;
static volatile int flush_pending; /* set by fbcon_flush_deferred() */
static volatile int flushing;      /* reentrant/concurrent guard */

/* VT100 parser state */
enum { ST_NORMAL, ST_ESC, ST_CSI };
static int vt_state;
static int vt_params[VT_MAX_PARAMS];
static int vt_nparams;
static int vt_private; /* non-zero if '?' seen after ESC [ */

/* Scroll region (0-based row indices, inclusive) */
static int scroll_top, scroll_bot;

/* ---------- internal helpers ---------- */

static void scroll_up_region(void) {
  /* Compare before memmove: only mark rows whose content actually changes.
   * Row r will receive row r+1's content, so compare them pairwise. */
  for (int r = scroll_top; r < scroll_bot; r++) {
    if (memcmp(cell_char[r], cell_char[r + 1], (size_t)cols) != 0 ||
        memcmp(cell_attr[r], cell_attr[r + 1], (size_t)cols) != 0)
      dirty[r] = 1;
  }
  memmove(cell_char[scroll_top], cell_char[scroll_top + 1],
          (uint32_t)(scroll_bot - scroll_top) * FBCON_MAX_COLS);
  memmove(cell_attr[scroll_top], cell_attr[scroll_top + 1],
          (uint32_t)(scroll_bot - scroll_top) * FBCON_MAX_COLS);
  /* Bottom row: only dirty if it wasn't already blank */
  for (int c = 0; c < cols; c++) {
    if (cell_char[scroll_bot][c] != ' ' ||
        cell_attr[scroll_bot][c] != cur_attr) {
      dirty[scroll_bot] = 1;
      break;
    }
  }
  memset(cell_char[scroll_bot], ' ', (uint32_t)cols);
  memset(cell_attr[scroll_bot], cur_attr, (uint32_t)cols);
}

static void scroll_down_region(void) {
  /* Compare before memmove: row r will receive row r-1's content. */
  for (int r = scroll_bot; r > scroll_top; r--) {
    if (memcmp(cell_char[r], cell_char[r - 1], (size_t)cols) != 0 ||
        memcmp(cell_attr[r], cell_attr[r - 1], (size_t)cols) != 0)
      dirty[r] = 1;
  }
  memmove(cell_char[scroll_top + 1], cell_char[scroll_top],
          (uint32_t)(scroll_bot - scroll_top) * FBCON_MAX_COLS);
  memmove(cell_attr[scroll_top + 1], cell_attr[scroll_top],
          (uint32_t)(scroll_bot - scroll_top) * FBCON_MAX_COLS);
  /* Top row: only dirty if it wasn't already blank */
  for (int c = 0; c < cols; c++) {
    if (cell_char[scroll_top][c] != ' ' ||
        cell_attr[scroll_top][c] != cur_attr) {
      dirty[scroll_top] = 1;
      break;
    }
  }
  memset(cell_char[scroll_top], ' ', (uint32_t)cols);
  memset(cell_attr[scroll_top], cur_attr, (uint32_t)cols);
}

/* Captured font parameters for render — protects against concurrent
 * fbcon_set_mode() changing globals mid-flush. */
typedef struct {
  int cols, rows, font_w, font_h, font_stride;
  const uint8_t *font_data;
} render_params_t;

/* Render one scanline of a row into buf[0..rp->cols*rp->font_w-1]. */
static void render_scanline(const render_params_t *rp, int row, int sy,
                            uint16_t *buf) {
  int width = rp->cols * rp->font_w;
  for (int c = 0; c < rp->cols; c++) {
    uint8_t ch = cell_char[row][c];
    uint8_t attr = cell_attr[row][c];
    uint16_t fg = ansi_palette[attr & 0x0F];
    uint16_t bg = ansi_palette[(attr >> 4) & 0x0F];
    uint8_t bits = rp->font_data[ch * rp->font_stride + sy];
    int base = c * rp->font_w;
    for (int px = 0; px < rp->font_w; px++) {
      buf[base + px] = (bits & (0x80 >> px)) ? fg : bg;
    }
  }
  /* Clear any remaining pixels if cols*font_w < LCD_WIDTH */
  for (int x = width; x < LCD_WIDTH; x++) buf[x] = 0x0000;
}

/* Clamp cursor to valid bounds */
static inline void clamp_cursor(void) {
  if (cursor_x < 0) cursor_x = 0;
  if (cursor_x >= cols) cursor_x = cols - 1;
  if (cursor_y < 0) cursor_y = 0;
  if (cursor_y >= rows) cursor_y = rows - 1;
}

/* Compute the effective attribute byte from cur_attr + bold + reverse */
static inline uint8_t effective_attr(void) {
  uint8_t a = cur_attr;
  if (bold) {
    int fg = a & 0x0F;
    if (fg < 8) fg += 8;
    a = (uint8_t)((a & 0xF0) | fg);
  }
  if (reverse) {
    a = (uint8_t)(((a & 0x0F) << 4) | ((a >> 4) & 0x0F));
  }
  return a;
}

/* ---------- CSI dispatch helpers ---------- */

static inline int param(int idx, int def) {
  if (idx >= vt_nparams || vt_params[idx] == 0) return def;
  return vt_params[idx];
}

static void csi_cursor_up(void) {
  cursor_y -= param(0, 1);
  clamp_cursor();
}

static void csi_cursor_down(void) {
  cursor_y += param(0, 1);
  clamp_cursor();
}

static void csi_cursor_forward(void) {
  cursor_x += param(0, 1);
  clamp_cursor();
}

static void csi_cursor_back(void) {
  cursor_x -= param(0, 1);
  clamp_cursor();
}

static void csi_cursor_position(void) {
  /* CSI row ; col H — 1-based, default 1 */
  cursor_y = param(0, 1) - 1;
  cursor_x = param(1, 1) - 1;
  clamp_cursor();
}

static void csi_erase_display(void) {
  int mode = param(0, 0);
  uint8_t a = effective_attr();
  if (mode == 0) {
    /* Erase from cursor to end of screen */
    for (int c = cursor_x; c < cols; c++) {
      cell_char[cursor_y][c] = ' ';
      cell_attr[cursor_y][c] = a;
    }
    dirty[cursor_y] = 1;
    for (int r = cursor_y + 1; r < rows; r++) {
      memset(cell_char[r], ' ', (uint32_t)cols);
      memset(cell_attr[r], a, (uint32_t)cols);
      dirty[r] = 1;
    }
  } else if (mode == 1) {
    /* Erase from start of screen to cursor */
    for (int r = 0; r < cursor_y; r++) {
      memset(cell_char[r], ' ', (uint32_t)cols);
      memset(cell_attr[r], a, (uint32_t)cols);
      dirty[r] = 1;
    }
    for (int c = 0; c <= cursor_x; c++) {
      cell_char[cursor_y][c] = ' ';
      cell_attr[cursor_y][c] = a;
    }
    dirty[cursor_y] = 1;
  } else if (mode == 2) {
    /* Erase entire screen */
    for (int r = 0; r < rows; r++) {
      memset(cell_char[r], ' ', (uint32_t)cols);
      memset(cell_attr[r], a, (uint32_t)cols);
      dirty[r] = 1;
    }
  }
}

static void csi_erase_line(void) {
  int mode = param(0, 0);
  uint8_t a = effective_attr();
  if (mode == 0) {
    /* Erase from cursor to end of line */
    for (int c = cursor_x; c < cols; c++) {
      cell_char[cursor_y][c] = ' ';
      cell_attr[cursor_y][c] = a;
    }
  } else if (mode == 1) {
    /* Erase from start of line to cursor */
    for (int c = 0; c <= cursor_x; c++) {
      cell_char[cursor_y][c] = ' ';
      cell_attr[cursor_y][c] = a;
    }
  } else if (mode == 2) {
    /* Erase entire line */
    memset(cell_char[cursor_y], ' ', (uint32_t)cols);
    memset(cell_attr[cursor_y], a, (uint32_t)cols);
  }
  dirty[cursor_y] = 1;
}

static void csi_sgr(void) {
  /* If no params, treat as SGR 0 (reset) */
  if (vt_nparams == 0) {
    cur_attr = 0x07;
    bold = 0;
    reverse = 0;
    return;
  }
  for (int i = 0; i < vt_nparams; i++) {
    int p = vt_params[i];
    if (p == 0) {
      cur_attr = 0x07;
      bold = 0;
      reverse = 0;
    } else if (p == 1) {
      bold = 1;
    } else if (p == 7) {
      reverse = 1;
    } else if (p == 22) {
      bold = 0;
    } else if (p == 27) {
      reverse = 0;
    } else if (p >= 30 && p <= 37) {
      cur_attr = (uint8_t)((cur_attr & 0xF0) | (p - 30));
    } else if (p == 39) {
      cur_attr = (uint8_t)((cur_attr & 0xF0) | 7);
    } else if (p >= 40 && p <= 47) {
      cur_attr = (uint8_t)((cur_attr & 0x0F) | ((p - 40) << 4));
    } else if (p == 49) {
      cur_attr = (uint8_t)(cur_attr & 0x0F);
    } else if (p >= 90 && p <= 97) {
      cur_attr = (uint8_t)((cur_attr & 0xF0) | (p - 90 + 8));
    } else if (p >= 100 && p <= 107) {
      cur_attr = (uint8_t)((cur_attr & 0x0F) | ((p - 100 + 8) << 4));
    }
  }
}

static void csi_set_scroll_region(void) {
  int top = param(0, 1) - 1;
  int bot = param(1, rows) - 1;
  if (top < 0) top = 0;
  if (bot >= rows) bot = rows - 1;
  if (top < bot) {
    scroll_top = top;
    scroll_bot = bot;
  }
  cursor_x = 0;
  cursor_y = 0;
}

static void csi_insert_lines(void) {
  int n = param(0, 1);
  if (cursor_y < scroll_top || cursor_y > scroll_bot) return;
  int avail = scroll_bot - cursor_y + 1;
  if (n > avail) n = avail;
  uint8_t a = effective_attr();
  /* Shift lines down by n within scroll region */
  for (int r = scroll_bot; r >= cursor_y + n; r--) {
    memcpy(cell_char[r], cell_char[r - n], (size_t)cols);
    memcpy(cell_attr[r], cell_attr[r - n], (size_t)cols);
    dirty[r] = 1;
  }
  /* Clear the inserted lines */
  for (int r = cursor_y; r < cursor_y + n; r++) {
    memset(cell_char[r], ' ', (size_t)cols);
    memset(cell_attr[r], a, (size_t)cols);
    dirty[r] = 1;
  }
  cursor_x = 0;
}

static void csi_delete_lines(void) {
  int n = param(0, 1);
  if (cursor_y < scroll_top || cursor_y > scroll_bot) return;
  int avail = scroll_bot - cursor_y + 1;
  if (n > avail) n = avail;
  uint8_t a = effective_attr();
  /* Shift lines up by n within scroll region */
  for (int r = cursor_y; r <= scroll_bot - n; r++) {
    memcpy(cell_char[r], cell_char[r + n], (size_t)cols);
    memcpy(cell_attr[r], cell_attr[r + n], (size_t)cols);
    dirty[r] = 1;
  }
  /* Clear the vacated lines at bottom */
  for (int r = scroll_bot - n + 1; r <= scroll_bot; r++) {
    memset(cell_char[r], ' ', (size_t)cols);
    memset(cell_attr[r], a, (size_t)cols);
    dirty[r] = 1;
  }
  cursor_x = 0;
}

static void csi_private_mode(int final) {
  int mode = param(0, 0);
  int set = (final == 'h'); /* h = set, l = reset */
  if (mode == 80) {
    /* Private mode 80: 80/40-col switch */
    fbcon_set_mode(set ? FBCON_MODE_COMPACT : FBCON_MODE_NORMAL);
  } else if (mode == 40) {
    /* Private mode 40: 40×40 square mode */
    fbcon_set_mode(set ? FBCON_MODE_SQUARE : FBCON_MODE_NORMAL);
  }
  /* ESC [ ? 25 h/l — cursor show/hide: accepted but no-op for now */
}

static void csi_dispatch(int final) {
  if (vt_private) {
    csi_private_mode(final);
    return;
  }
  switch (final) {
    case 'A':
      csi_cursor_up();
      break;
    case 'B':
      csi_cursor_down();
      break;
    case 'C':
      csi_cursor_forward();
      break;
    case 'D':
      csi_cursor_back();
      break;
    case 'H': /* fall through */
    case 'f':
      csi_cursor_position();
      break;
    case 'J':
      csi_erase_display();
      break;
    case 'K':
      csi_erase_line();
      break;
    case 'L':
      csi_insert_lines();
      break;
    case 'M':
      csi_delete_lines();
      break;
    case 'm':
      csi_sgr();
      break;
    case 'r':
      csi_set_scroll_region();
      break;
    default:
      break; /* unknown sequence — silently ignore */
  }
}

/* ---------- character output (handles newline + scroll region) ---------- */

static void emit_newline(void) {
  cursor_x = 0;
  if (cursor_y == scroll_bot) {
    scroll_up_region();
  } else if (cursor_y < rows - 1) {
    cursor_y++;
  }
}

static void emit_printable(uint8_t ch) {
  cell_char[cursor_y][cursor_x] = ch;
  cell_attr[cursor_y][cursor_x] = effective_attr();
  dirty[cursor_y] = 1;
  cursor_x++;
  if (cursor_x >= cols) {
    cursor_x = 0;
    if (cursor_y == scroll_bot) {
      scroll_up_region();
    } else if (cursor_y < rows - 1) {
      cursor_y++;
    }
  }
}

/* ---------- public API ---------- */

void fbcon_init(void) {
  vt_state = ST_NORMAL;
  bold = 0;
  /* Set mode directly and flush immediately — at init time the scheduler
   * hasn't started so there's no concurrency and the deferred path
   * (idle loop) isn't running yet. */
  fbcon_set_mode(FBCON_MODE_NORMAL);
  fbcon_flush(); /* render the initial blank screen now */
}

int fbcon_putc(char c, void (*notify)(void)) {
  (void)notify;
  uint8_t ch = (uint8_t)c;

  switch (vt_state) {
    case ST_NORMAL:
      if (ch == '\033') {
        vt_state = ST_ESC;
      } else if (ch >= 0x20 && ch <= 0x7E) {
        emit_printable(ch);
      } else if (ch == '\n') {
        emit_newline();
      } else if (ch == '\r') {
        cursor_x = 0;
      } else if (ch == '\b') {
        if (cursor_x > 0) cursor_x--;
      } else if (ch == '\t') {
        int next = (cursor_x + 8) & ~7;
        if (next >= cols) next = cols - 1;
        cursor_x = next;
      }
      break;

    case ST_ESC:
      if (ch == '[') {
        /* Enter CSI sequence */
        vt_state = ST_CSI;
        vt_nparams = 0;
        vt_private = 0;
        for (int i = 0; i < VT_MAX_PARAMS; i++) vt_params[i] = 0;
      } else if (ch == 'c') {
        /* RIS — full reset */
        vt_state = ST_NORMAL;
        bold = 0;
        reverse = 0;
        cur_attr = 0x07;
        scroll_top = 0;
        scroll_bot = rows - 1;
        fbcon_clear();
      } else if (ch == 'M') {
        /* RI — Reverse Index: move cursor up; scroll down at top */
        vt_state = ST_NORMAL;
        if (cursor_y == scroll_top) {
          scroll_down_region();
        } else if (cursor_y > 0) {
          cursor_y--;
        }
      } else if (ch == '7') {
        /* DECSC — Save cursor position + attributes */
        vt_state = ST_NORMAL;
        saved_x = cursor_x;
        saved_y = cursor_y;
        saved_attr = cur_attr;
        saved_bold = bold;
        saved_reverse = reverse;
      } else if (ch == '8') {
        /* DECRC — Restore cursor position + attributes */
        vt_state = ST_NORMAL;
        cursor_x = saved_x;
        cursor_y = saved_y;
        cur_attr = saved_attr;
        bold = saved_bold;
        reverse = saved_reverse;
        clamp_cursor();
      } else {
        /* Unknown ESC sequence — discard and return to normal */
        vt_state = ST_NORMAL;
      }
      break;

    case ST_CSI:
      if (ch == '?') {
        vt_private = 1;
      } else if (ch >= '0' && ch <= '9') {
        /* Accumulate numeric parameter */
        if (vt_nparams == 0) vt_nparams = 1;
        vt_params[vt_nparams - 1] = vt_params[vt_nparams - 1] * 10 + (ch - '0');
      } else if (ch == ';') {
        /* Next parameter */
        if (vt_nparams < VT_MAX_PARAMS) vt_nparams++;
      } else if (ch >= 0x40 && ch <= 0x7E) {
        /* Final byte — dispatch and return to normal */
        if (vt_nparams == 0 && ch != 'm')
          vt_nparams = 1; /* ensure at least 1 param slot */
        vt_state = ST_NORMAL;
        csi_dispatch(ch);
      } else {
        /* Unexpected byte in CSI — abort */
        vt_state = ST_NORMAL;
      }
      break;
  }
  flush_pending = 1;
  return 1;
}

void fbcon_puts(const char *s) {
  while (*s) fbcon_putc(*s++, NULL);
}

void fbcon_flush(void) {
  /* If SPI1 has timed out (lcd_ok=0), all spi_lcd_* ops are no-ops.
   * Skip the flush entirely to avoid burning CPU in dead loops. */
  if (!spi_lcd_ok()) return;

  /* Guard against concurrent flush from two cores.  Two simultaneous
   * SPI1 users corrupt the bus → permanent hang.
   *
   * This is NOT a simple volatile test — we use __atomic_test_and_set
   * (GCC built-in, maps to LDREX/STREX on ARMv7+ or a plain byte
   * store on ARMv6-M).  On Cortex-M0+ (no LDREX), the race window is
   * a single instruction; combined with the fact that only Core 0's
   * idle loop calls this, the risk of double-entry is negligible.
   * The deferred re-arm ensures correctness even if it does happen. */
  if (__atomic_test_and_set(&flushing, __ATOMIC_ACQUIRE)) {
    flush_pending = 1;
    return;
  }

  /* Snapshot render parameters and dirty flags before the slow SPI render.
   * fbcon_putc() or fbcon_set_mode() on the other core can change cols,
   * font_data, font_stride etc. at any time.  Using stale-but-consistent
   * values produces at most one frame of visual tearing, which is fine.
   *
   * Dirty flags are cleared upfront: if fbcon_putc() sets dirty[r]=1
   * during our render, that flag survives and the next poll picks it up.
   * The old approach (clear after render) could lose a concurrent set. */
  render_params_t rp = {
      .cols = cols,
      .rows = rows,
      .font_w = font_w,
      .font_h = font_h,
      .font_stride = font_stride,
      .font_data = font_data,
  };
  uint8_t snap[FBCON_MAX_ROWS];
  for (int r = 0; r < rp.rows; r++) {
    snap[r] = dirty[r];
    if (snap[r]) dirty[r] = 0;
  }

  /* Stack buffer for one scanline — 320 pixels × 2 bytes = 640 B */
  uint16_t line[LCD_WIDTH];

  for (int r = 0; r < rp.rows; r++) {
    if (!snap[r]) continue;
    /* Set window for the entire row, then stream all scanlines with
     * CS held low.  Saves ~15 set_window round-trips per row. */
    uint16_t y0 = (uint16_t)(r * rp.font_h);
    uint16_t y1 = (uint16_t)(y0 + rp.font_h - 1);
    spi_lcd_set_window(0, y0, (uint16_t)(LCD_WIDTH - 1), y1);
    spi_lcd_stream_begin();
    for (int sy = 0; sy < rp.font_h; sy++) {
      render_scanline(&rp, r, sy, line);
      spi_lcd_data16_stream(line, LCD_WIDTH);
    }
    spi_lcd_stream_end();
  }

  __atomic_clear(&flushing, __ATOMIC_RELEASE);
}

void fbcon_flush_deferred(void) { flush_pending = 1; }

void fbcon_poll_flush(void) {
  if (flush_pending) {
    flush_pending = 0;
    fbcon_flush();
  }
}

void fbcon_clear(void) {
  memset(cell_char, ' ', sizeof(cell_char));
  memset(cell_attr, cur_attr, sizeof(cell_attr));
  cursor_x = 0;
  cursor_y = 0;
  for (int r = 0; r < rows; r++) dirty[r] = 1;
}

void fbcon_set_cursor(int x, int y) {
  cursor_x = x;
  cursor_y = y;
  clamp_cursor();
}

void fbcon_set_mode(int mode) {
  int old_cols = cols;
  int old_rows = rows;

  if (mode == FBCON_MODE_COMPACT) {
    cols = 80;
    rows = 40;
    font_w = 4;
    font_h = 8;
    font_stride = 8;
    font_data = &font4x8[0][0];
  } else if (mode == FBCON_MODE_SQUARE) {
    cols = 40;
    rows = 40;
    font_w = 8;
    font_h = 8;
    font_stride = 8;
    font_data = &font8x8[0][0];
  } else {
    cols = 40;
    rows = 20;
    font_w = 8;
    font_h = 16;
    font_stride = 16;
    font_data = &font8x16[0][0];
  }
  cur_attr = 0x07; /* white on black */
  bold = 0;
  reverse = 0;
  vt_state = ST_NORMAL;
  scroll_top = 0;
  scroll_bot = rows - 1;

  /* Clear newly-visible cells when expanding */
  if (cols > old_cols) {
    for (int r = 0; r < old_rows && r < rows; r++) {
      memset(&cell_char[r][old_cols], ' ', (size_t)(cols - old_cols));
      memset(&cell_attr[r][old_cols], cur_attr, (size_t)(cols - old_cols));
    }
  }
  if (rows > old_rows) {
    for (int r = old_rows; r < rows; r++) {
      memset(cell_char[r], ' ', (size_t)cols);
      memset(cell_attr[r], cur_attr, (size_t)cols);
    }
  }

  /* Clamp cursor to new grid */
  if (cursor_x >= cols) cursor_x = cols - 1;
  if (cursor_y >= rows) cursor_y = rows - 1;

  /* Redraw everything with the new font.
   * Use deferred flush — calling fbcon_flush() directly here would run
   * a long SPI transfer inside the syscall context, and if the idle
   * loop on the other core is also flushing we'd have two concurrent
   * SPI1 users → bus corruption → hang.  The idle loop picks this up. */
  for (int r = 0; r < rows; r++) dirty[r] = 1;
  fbcon_flush_deferred();
}

int fbcon_cols(void) { return cols; }
int fbcon_rows(void) { return rows; }

void fbcon_set_attr(uint8_t attr) { cur_attr = attr; }
