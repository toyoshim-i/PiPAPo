/*
 * pile_draw.c — VT100 rendering for the pile filer
 *
 * Design: docs/proposals/pile.md
 *
 * Chrome layout (identical for single-pane and two-pane modes, 7 rows):
 *   row 0                   pane header(s)
 *   rows 1..vrows           entry list
 *   row vrows + 1           per-pane footer (cursor/count, size total)
 *   row vrows + 2           horizontal rule
 *   row vrows + 3           stat strip line 1: full file path
 *   row vrows + 4           stat strip line 2: size / mode / mtime
 *   row vrows + 5           horizontal rule
 *   row vrows + 6           key legend
 *
 * visible_rows = pile_rows - 7.  The vertical divider in two-pane mode
 * spans rows 0 .. vrows+1 (header through per-pane footer); the stat
 * strip and legend span the full width below it.
 */

#include "pile.h"

#define C(seq) (pile_use_color ? (seq) : "")
#define C_RST      C("\033[0m")
#define C_DIR      C("\033[1;34m")
#define C_EXEC     C("\033[1;32m")
#define C_LINK     C("\033[36m")
#define C_DEV      C("\033[1;33m")
#define C_CUR      C("\033[1;7m")
#define C_CUR_OFF  C("\033[7m")
#define C_FRAME    C("\033[2m")
#define C_HEADER   C("\033[1m")
#define C_HEADER_OFF C("\033[2m")
#define C_KEY      C("\033[1m")

#define CHROME_ROWS 7

/* ── Low-level ANSI helpers ───────────────────────────────────────────── */

static char out_buf[32];

static void emit(const char *s) {
  int len = uc_strlen(s);
  if (len > 0) write(1, s, len);
}

static void put_uint(int v) {
  char buf[12];
  int pos = (int)sizeof(buf);
  buf[--pos] = '\0';
  if (v == 0) {
    buf[--pos] = '0';
  } else {
    int neg = v < 0;
    unsigned u = (unsigned)(neg ? -v : v);
    while (u && pos > 0) {
      buf[--pos] = (char)('0' + u % 10);
      u /= 10;
    }
    if (neg && pos > 0) buf[--pos] = '-';
  }
  emit(buf + pos);
}

static void cursor_to(int row, int col) {
  char *p = out_buf;
  *p++ = '\033';
  *p++ = '[';
  int r = row + 1, c = col + 1;
  if (r >= 100) *p++ = (char)('0' + r / 100);
  if (r >= 10)  *p++ = (char)('0' + (r / 10) % 10);
  *p++ = (char)('0' + r % 10);
  *p++ = ';';
  if (c >= 100) *p++ = (char)('0' + c / 100);
  if (c >= 10)  *p++ = (char)('0' + (c / 10) % 10);
  *p++ = (char)('0' + c % 10);
  *p++ = 'H';
  write(1, out_buf, (int)(p - out_buf));
}

static void clear_screen(void) { emit("\033[2J\033[H"); }
static void clear_to_eol(void) { emit("\033[K"); }
static void cursor_hide(void) { emit("\033[?25l"); }
static void cursor_show(void) { emit("\033[?25h"); }
static void attr_reset(void) { emit("\033[0m"); }

static void put_spaces(int n) {
  for (int i = 0; i < n; i++) uc_putc(' ');
}

/* ── Public helpers ───────────────────────────────────────────────────── */

int pile_draw_visible_rows(void) {
  int v = pile_rows - CHROME_ROWS;
  if (v < 1) v = 1;
  return v;
}

/* ── Size / mode formatting ───────────────────────────────────────────── */

static void fmt_size(char buf[10], const pile_entry_t *e) {
  if (e->d_type == DT_DIR) {
    const char *s = "    <DIR>";
    for (int i = 0; i < 9; i++) buf[i] = s[i];
    buf[9] = '\0';
    return;
  }
  if (e->d_type == DT_CHR) {
    const char *s = "    <DEV>";
    for (int i = 0; i < 9; i++) buf[i] = s[i];
    buf[9] = '\0';
    return;
  }
  uint32_t v = e->size;
  int pos = 9;
  buf[pos] = '\0';
  if (v == 0) {
    buf[--pos] = '0';
  } else {
    while (v && pos > 0) {
      buf[--pos] = (char)('0' + v % 10);
      v /= 10;
    }
  }
  while (pos > 0) buf[--pos] = ' ';
}

/* Compact form for pane-footer totals, width-7 right-justified:
 * decimal below 10K, K suffix up to 10M, M suffix above. */
static void fmt_size_compact(char buf[8], uint32_t v) {
  const char *suffix = "";
  if (v >= 10UL * 1024UL * 1024UL) {
    v /= (1024UL * 1024UL);
    suffix = "M";
  } else if (v >= 10UL * 1024UL) {
    v /= 1024UL;
    suffix = "K";
  }
  char digits[11];
  int dpos = (int)sizeof(digits);
  digits[--dpos] = '\0';
  if (v == 0) digits[--dpos] = '0';
  while (v && dpos > 0) {
    digits[--dpos] = (char)('0' + v % 10);
    v /= 10;
  }
  int dlen = (int)sizeof(digits) - 1 - dpos;
  int slen = uc_strlen(suffix);
  int pad = 7 - dlen - slen;
  if (pad < 0) pad = 0;
  int out = 0;
  while (pad-- > 0 && out < 7) buf[out++] = ' ';
  for (int i = 0; i < dlen && out < 7; i++) buf[out++] = digits[dpos + i];
  for (int i = 0; i < slen && out < 7; i++) buf[out++] = suffix[i];
  buf[out] = '\0';
}

static void fmt_mode(char buf[11], uint32_t mode, uint8_t d_type) {
  buf[0] = (d_type == DT_DIR) ? 'd'
         : (d_type == DT_LNK) ? 'l'
         : (d_type == DT_CHR) ? 'c'
                              : '-';
  buf[1] = (mode & 0400) ? 'r' : '-';
  buf[2] = (mode & 0200) ? 'w' : '-';
  buf[3] = (mode & 0100) ? 'x' : '-';
  buf[4] = (mode & 040)  ? 'r' : '-';
  buf[5] = (mode & 020)  ? 'w' : '-';
  buf[6] = (mode & 010)  ? 'x' : '-';
  buf[7] = (mode & 04)   ? 'r' : '-';
  buf[8] = (mode & 02)   ? 'w' : '-';
  buf[9] = (mode & 01)   ? 'x' : '-';
  buf[10] = '\0';
}

/* ── Entry coloring ───────────────────────────────────────────────────── */

static const char *entry_color(const pile_entry_t *e) {
  switch (e->d_type) {
    case DT_DIR: return C_DIR;
    case DT_LNK: return C_LINK;
    case DT_CHR: return C_DEV;
  }
  if (e->mode & 0111) return C_EXEC;
  return "";
}

static char entry_suffix(const pile_entry_t *e) {
  switch (e->d_type) {
    case DT_DIR: return '/';
    case DT_LNK: return '@';
  }
  if (e->mode & 0111) return '*';
  return ' ';
}

/* ── Pane-internal rendering ──────────────────────────────────────────── */

/* Emits a header line "── /path " followed by frame fill to width.
 * Active pane uses bold; inactive dims. */
static void draw_pane_header(int row, int col, int width,
                             const pile_pane_t *pane, int is_active) {
  cursor_to(row, col);
  emit(is_active ? C_HEADER : C_HEADER_OFF);
  emit("-- ");
  int used = 3;
  int plen = uc_strlen(pane->path);
  int max_path = width - 5;
  if (max_path < 4) max_path = 4;
  if (plen > max_path) {
    /* Truncate the head of the path — keep the tail visible. */
    uc_putc('~');
    used++;
    const char *tail = pane->path + (plen - (max_path - 1));
    int tail_len = max_path - 1;
    for (int i = 0; i < tail_len; i++) uc_putc(tail[i]);
    used += tail_len;
  } else {
    emit(pane->path);
    used += plen;
  }
  uc_putc(' ');
  used++;
  emit(C_RST);
  emit(C_FRAME);
  while (used < width) { uc_putc('-'); used++; }
  emit(C_RST);
}

static void draw_entry_row(int row, int col, int width,
                           const pile_pane_t *pane, int idx,
                           int is_cursor_active, int is_cursor_inactive) {
  cursor_to(row, col);
  if (idx < 0 || idx >= pane->count) {
    put_spaces(width);
    return;
  }
  const pile_entry_t *e = &pane->entries[idx];

  int is_cur = is_cursor_active || is_cursor_inactive;
  if (is_cursor_active) emit(C_CUR);
  else if (is_cursor_inactive) emit(C_CUR_OFF);

  uc_putc(' ');  /* gutter (reserved for mark glyph in P2b) */

  /* name | suffix | space | 9-col size */
  int name_col = width - 1 - 1 - 1 - 9;
  if (name_col < 4) name_col = 4;

  if (!is_cur) emit(entry_color(e));
  int nlen = uc_strlen(e->name);
  int shown = nlen < name_col ? nlen : name_col;
  for (int i = 0; i < shown; i++) uc_putc(e->name[i]);
  if (!is_cur) emit(C_RST);

  int pad = name_col - shown;
  put_spaces(pad);
  uc_putc(entry_suffix(e));
  uc_putc(' ');

  char sbuf[10];
  fmt_size(sbuf, e);
  emit(sbuf);

  if (is_cur) emit(C_RST);
}

/* Pane footer row: "  i/n      total" in compact form.  The marking
 * count (" X sel") lands in P2b. */
static void draw_pane_footer(int row, int col, int width,
                             const pile_pane_t *pane) {
  cursor_to(row, col);
  emit(C_FRAME);
  put_spaces(2);
  put_uint(pane->cursor + (pane->count ? 1 : 0));
  uc_putc('/');
  put_uint(pane->count);
  if (pane->truncated) uc_putc('+');

  uint32_t total = 0;
  for (int i = 0; i < pane->count; i++) {
    if (pane->entries[i].d_type == DT_REG) total += pane->entries[i].size;
  }
  char sz[8];
  fmt_size_compact(sz, total);

  /* Right-align total to the pane's right edge (leave 1 col of gutter). */
  int used_here = 2 + 1 + 1;  /* two leading spaces + "n/" counter approx */
  (void)used_here;
  /* Recompute used by writing what we know: simpler to just clear and
   * place the total at a fixed offset from the pane's right edge. */
  emit(C_RST);
  int slen = uc_strlen(sz);
  int total_col = col + width - 1 - slen;
  cursor_to(row, total_col);
  emit(C_FRAME);
  emit(sz);
  emit(C_RST);
}

/* ── Divider column ───────────────────────────────────────────────────── */

static void draw_divider(int col, int first_row, int last_row) {
  emit(C_FRAME);
  for (int r = first_row; r <= last_row; r++) {
    cursor_to(r, col);
    uc_putc('|');
  }
  emit(C_RST);
}

/* ── Global strips ────────────────────────────────────────────────────── */

static void draw_rule(int row) {
  cursor_to(row, 0);
  emit(C_FRAME);
  for (int i = 0; i < pile_cols; i++) uc_putc('-');
  emit(C_RST);
}

static void draw_stat_strip(int row_path, int row_detail) {
  const pile_pane_t *pane = pile_active;

  cursor_to(row_path, 0);
  if (pane->count == 0) {
    emit(C_FRAME);
    emit(" (empty directory)");
    emit(C_RST);
    clear_to_eol();
    cursor_to(row_detail, 0);
    clear_to_eol();
    return;
  }

  const pile_entry_t *e = &pane->entries[pane->cursor];

  /* Line 1: leading "  " + pane path + "/" + name */
  uc_putc(' ');
  uc_putc(' ');
  emit(pane->path);
  int plen = uc_strlen(pane->path);
  if (plen == 0 || pane->path[plen - 1] != '/') uc_putc('/');
  emit(entry_color(e));
  emit(e->name);
  emit(C_RST);
  clear_to_eol();

  /* Line 2: size + mode + mtime (if available) */
  cursor_to(row_detail, 0);
  uc_putc(' ');
  uc_putc(' ');
  char sbuf[10];
  fmt_size(sbuf, e);
  /* trim leading spaces from the right-justified size */
  int sp = 0;
  while (sbuf[sp] == ' ') sp++;
  emit(sbuf + sp);
  emit("  ");
  char mode_buf[11];
  fmt_mode(mode_buf, e->mode, e->d_type);
  emit(mode_buf);
  if (e->mtime) {
    emit("  ");
    char tbuf[17];
    uc_format_ymdhm(tbuf, e->mtime);
    emit(C_FRAME);
    emit(tbuf);
    emit(C_RST);
  }
  clear_to_eol();
}

static void draw_legend(int row) {
  cursor_to(row, 0);
  uc_putc(' ');
  emit(C_KEY); emit("TAB");   emit(C_RST); emit(" switch  ");
  emit(C_KEY); emit("ENTER"); emit(C_RST); emit(" open  ");
  emit(C_KEY); emit("BS");    emit(C_RST); emit(" parent  ");
  emit(C_KEY); emit("F10");   emit(C_RST); uc_putc('/');
  emit(C_KEY); emit("q");     emit(C_RST); emit(" quit");
  clear_to_eol();
}

/* ── Pane renderer ────────────────────────────────────────────────────── */

static void draw_pane(const pile_pane_t *pane, int col, int width,
                      int is_active, int vrows) {
  /* Header. */
  draw_pane_header(0, col, width, pane, is_active);

  /* Entries. */
  for (int i = 0; i < vrows; i++) {
    int idx = pane->scroll + i;
    int is_cur = (idx == pane->cursor) && idx < pane->count;
    draw_entry_row(1 + i, col, width, pane, idx,
                   is_active && is_cur, !is_active && is_cur);
  }

  /* Per-pane footer. */
  cursor_to(1 + vrows, col);
  put_spaces(width);
  draw_pane_footer(1 + vrows, col, width, pane);
}

/* ── Public entrypoints ───────────────────────────────────────────────── */

void pile_draw_all(void) {
  cursor_hide();
  int vrows = pile_draw_visible_rows();

  if (pile_layout == PILE_LAYOUT_TWO) {
    int lw = pile_cols / 2;
    int rw = pile_cols - lw - 1;  /* one column for the divider */
    int rs = lw + 1;
    draw_pane(&pile_pane_a, 0, lw, pile_active == &pile_pane_a, vrows);
    draw_pane(&pile_pane_b, rs, rw, pile_active == &pile_pane_b, vrows);
    draw_divider(lw, 0, 1 + vrows);
  } else {
    draw_pane(pile_active, 0, pile_cols, 1, vrows);
  }

  /* Strips below the list area. */
  draw_rule(1 + vrows + 1);
  draw_stat_strip(1 + vrows + 2, 1 + vrows + 3);
  draw_rule(1 + vrows + 4);
  draw_legend(1 + vrows + 5);

  /* Park the cursor at the bottom-right so it doesn't blink in the list. */
  cursor_to(pile_rows - 1, pile_cols - 1);
  cursor_show();
}

void pile_draw_clear(void) {
  attr_reset();
  clear_screen();
  cursor_to(0, 0);
  cursor_show();
}
