/*
 * calc_segdisp.c — 7-segment / LED-dot display layout
 *
 * Renders a calc_value_str_t into one or three lines of fixed-width text.
 *   - DEC / HEX / OCT: 3-line 7-seg glyphs, abutting (3 cols/digit).
 *   - BIN:             1-line LED-dot row, '*' lit and '.' unlit, with a
 *                      single space between every nibble.
 * Falls back to plain grouped text if the chosen form does not fit
 * `max_cols`.  Output is right-aligned by left-padding with spaces.
 *
 * Pure: no syscalls, no IO.  Exercised by tests/host/test_calc_segdisp.c.
 */

#include "calc.h"

/* ── 7-seg glyph table (3 rows × 3 cols per digit) ──────────────────────── */

/* Indexed by:
 *   0..9  -> digits '0'..'9'
 *   10    -> 'A'
 *   11    -> 'b'  (lowercase to disambiguate from '8')
 *   12    -> 'C'
 *   13    -> 'd'  (lowercase to disambiguate from '0')
 *   14    -> 'E'
 *   15    -> 'F'
 *   16    -> minus sign
 */
#define SEG_NDIGIT  16
#define SEG_MINUS   16
#define SEG_GLYPHS  17
#define SEG_GLYPH_W 3

static const char SEG_GLYPH[SEG_GLYPHS][3][SEG_GLYPH_W + 1] = {
    /* 0 */ {" _ ", "| |", "|_|"},
    /* 1 */ {"   ", "  |", "  |"},
    /* 2 */ {" _ ", " _|", "|_ "},
    /* 3 */ {" _ ", " _|", " _|"},
    /* 4 */ {"   ", "|_|", "  |"},
    /* 5 */ {" _ ", "|_ ", " _|"},
    /* 6 */ {" _ ", "|_ ", "|_|"},
    /* 7 */ {" _ ", "  |", "  |"},
    /* 8 */ {" _ ", "|_|", "|_|"},
    /* 9 */ {" _ ", "|_|", " _|"},
    /* A */ {" _ ", "|_|", "| |"},
    /* b */ {"   ", "|_ ", "|_|"},
    /* C */ {" _ ", "|  ", "|_ "},
    /* d */ {"   ", " _|", "|_|"},
    /* E */ {" _ ", "|_ ", "|_ "},
    /* F */ {" _ ", "|_ ", "|  "},
    /* - */ {"   ", " _ ", "   "},
};

static int glyph_index(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c == 'A') return 10;
  if (c == 'B') return 11;  /* render as lowercase b */
  if (c == 'C') return 12;
  if (c == 'D') return 13;  /* render as lowercase d */
  if (c == 'E') return 14;
  if (c == 'F') return 15;
  if (c == '-') return SEG_MINUS;
  return 0;  /* shouldn't reach */
}

/* ── Width calculations for fit decisions ───────────────────────────────── */

static int seg_total_width(const calc_value_str_t *v) {
  int n = v->len + (v->negative ? 1 : 0);
  return n * SEG_GLYPH_W;  /* digits abut, no inter-digit gap */
}

/* LED-dot width for BIN: one cell per bit + one space between every group
 * of 4 bits.  v->len for BIN is already padded to the calculator's width. */
static int led_total_width(const calc_value_str_t *v) {
  int bits = v->len;
  if (bits <= 0) return 0;
  int gaps = (bits - 1) / 4;
  return bits + gaps;
}

/* ── Output helpers ─────────────────────────────────────────────────────── */

static void clear_lines(calc_disp_t *out) {
  for (int r = 0; r < CALC_DISP_MAX_LINES; r++)
    out->lines[r][0] = '\0';
  out->line_count = 0;
  out->visible_w  = 0;
  out->used_seg   = 0;
}

/* Right-align `content_w` chars within `field_w` by writing `pad` spaces
 * to `dst`, then return dst + pad.  Caller writes content after. */
static char *right_pad(char *dst, int content_w, int field_w) {
  int pad = field_w - content_w;
  if (pad < 0) pad = 0;
  for (int i = 0; i < pad; i++)
    dst[i] = ' ';
  return dst + pad;
}

/* ── Plain text fallback ────────────────────────────────────────────────── */

static void render_plain(const calc_value_str_t *v, int max_cols,
                         calc_disp_t *out) {
  char tmp[CALC_DISP_MAX_LINE];
  int w = calc_render_grouped(v, tmp, sizeof(tmp));
  /* If the grouped form is wider than max_cols we still show it left-
   * truncated rather than refusing — UI calls are responsible for the
   * higher-level "shrink width" hint. */
  if (w > max_cols)
    w = max_cols;
  if (max_cols >= CALC_DISP_MAX_LINE)
    max_cols = CALC_DISP_MAX_LINE - 1;
  char *p = right_pad(out->lines[0], w, max_cols);
  for (int i = 0; i < w; i++)
    p[i] = tmp[i];
  out->lines[0][max_cols] = '\0';
  out->line_count = 1;
  out->visible_w  = max_cols;
  out->used_seg   = 0;
}

/* ── 7-seg renderer for DEC/HEX/OCT ─────────────────────────────────────── */

static void render_seg(const calc_value_str_t *v, int max_cols,
                       calc_disp_t *out) {
  int total_digits = v->len + (v->negative ? 1 : 0);
  int content_w = total_digits * SEG_GLYPH_W;
  if (max_cols >= CALC_DISP_MAX_LINE)
    max_cols = CALC_DISP_MAX_LINE - 1;
  if (content_w > max_cols)
    content_w = max_cols;  /* shouldn't happen — caller checked fit */

  for (int row = 0; row < 3; row++) {
    char *p = right_pad(out->lines[row], content_w, max_cols);
    int wpos = 0;
    if (v->negative && wpos + SEG_GLYPH_W <= content_w) {
      const char *g = SEG_GLYPH[SEG_MINUS][row];
      for (int c = 0; c < SEG_GLYPH_W; c++) p[wpos++] = g[c];
    }
    for (int i = 0; i < v->len; i++) {
      if (wpos + SEG_GLYPH_W > content_w)
        break;
      const char *g = SEG_GLYPH[glyph_index(v->digits[i])][row];
      for (int c = 0; c < SEG_GLYPH_W; c++) p[wpos++] = g[c];
    }
    out->lines[row][max_cols] = '\0';
  }
  out->line_count = 3;
  out->visible_w  = max_cols;
  out->used_seg   = 1;
}

/* ── LED-dot row for BIN ────────────────────────────────────────────────── */

static void render_led(const calc_value_str_t *v, int max_cols,
                       calc_disp_t *out) {
  int bits = v->len;
  int content_w = led_total_width(v);
  if (max_cols >= CALC_DISP_MAX_LINE)
    max_cols = CALC_DISP_MAX_LINE - 1;
  if (content_w > max_cols)
    content_w = max_cols;

  char *p = right_pad(out->lines[0], content_w, max_cols);
  int wpos = 0;
  for (int i = 0; i < bits && wpos < content_w; i++) {
    if (i > 0 && (i % 4) == 0 && wpos < content_w)
      p[wpos++] = ' ';
    if (wpos < content_w)
      p[wpos++] = (v->digits[i] == '1') ? '*' : '.';
  }
  out->lines[0][max_cols] = '\0';
  out->line_count = 1;
  out->visible_w  = max_cols;
  out->used_seg   = 1;
}

/* ── Public entry point ─────────────────────────────────────────────────── */

void calc_segdisp_render(const calc_value_str_t *v, int max_cols,
                         int force_text, calc_disp_t *out) {
  clear_lines(out);
  if (max_cols <= 0)
    max_cols = 1;
  if (max_cols >= CALC_DISP_MAX_LINE)
    max_cols = CALC_DISP_MAX_LINE - 1;

  if (force_text) {
    render_plain(v, max_cols, out);
    return;
  }

  if (v->base == CALC_BASE_BIN) {
    if (led_total_width(v) <= max_cols) {
      render_led(v, max_cols, out);
      return;
    }
  } else {
    if (seg_total_width(v) <= max_cols) {
      render_seg(v, max_cols, out);
      return;
    }
  }
  render_plain(v, max_cols, out);
}
