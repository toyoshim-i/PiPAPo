/*
 * pile_draw.c — VT100 rendering for the pile filer
 *
 * Design: docs/proposals/pile.md
 *
 * P1 renders the single-pane layout.  The helper functions accept a
 * pile_pane_t* so the P2 two-pane expansion only needs to call them
 * twice with different column offsets.
 */

#include "pile.h"

#define C(seq) (pile_use_color ? (seq) : "")
#define C_RST     C("\033[0m")
#define C_DIR     C("\033[1;34m")
#define C_EXEC    C("\033[1;32m")
#define C_LINK    C("\033[36m")
#define C_DEV     C("\033[1;33m")
#define C_CUR     C("\033[1;7m")
#define C_FRAME   C("\033[2m")
#define C_HEADER  C("\033[1m")
#define C_KEY     C("\033[1m")

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

/* ── Size formatting ──────────────────────────────────────────────────── */

/* Right-justified in 9 cols: "<DIR>" for dirs, otherwise decimal size
 * with K/M suffix once it no longer fits in 9 digits. */
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
  /* Plain decimal for now; K/M suffixes can come in P2. */
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

/* Single-character trailing indicator (ls -F style) so the non-color
 * fallback still distinguishes types. */
static char entry_suffix(const pile_entry_t *e) {
  switch (e->d_type) {
    case DT_DIR: return '/';
    case DT_LNK: return '@';
  }
  if (e->mode & 0111) return '*';
  return ' ';
}

/* ── Lines ─────────────────────────────────────────────────────────────── */

static void draw_path_header(int row, int col, int width,
                             const pile_pane_t *pane) {
  cursor_to(row, col);
  emit(C_HEADER);
  /* "─── /path ─────────────" */
  emit("── ");
  int used = 3;
  int plen = uc_strlen(pane->path);
  int max_path = width - 6;  /* leave room for trailing "─" */
  if (max_path < 4) max_path = 4;
  if (plen > max_path) {
    /* Truncate left side: "…tail" */
    emit("…");
    used += 1;
    const char *tail = pane->path + (plen - (max_path - 1));
    emit(tail);
    used += max_path - 1;
  } else {
    emit(pane->path);
    used += plen;
  }
  emit(" ");
  used += 1;
  emit(C_FRAME);
  while (used < width) { uc_putc('-'); used++; }
  emit(C_RST);
}

static void draw_entry_row(int row, int col, int width,
                           const pile_pane_t *pane, int idx, int is_cursor) {
  cursor_to(row, col);
  if (idx < 0 || idx >= pane->count) {
    for (int i = 0; i < width; i++) uc_putc(' ');
    return;
  }
  const pile_entry_t *e = &pane->entries[idx];

  if (is_cursor) emit(C_CUR);
  uc_putc(' ');  /* leading gutter (reserved for mark glyph in P2) */

  /* Name column = width - 1 (gutter) - 9 (size) - 1 (trailing suffix)
   *              - 1 (space between). */
  int name_col = width - 1 - 9 - 1 - 1;
  if (name_col < 4) name_col = 4;

  if (!is_cursor) emit(entry_color(e));
  int nlen = uc_strlen(e->name);
  int shown = nlen < name_col ? nlen : name_col;
  for (int i = 0; i < shown; i++) uc_putc(e->name[i]);
  if (!is_cursor) emit(C_RST);

  /* Pad to reach the size column. */
  int pad = name_col - shown;
  for (int i = 0; i < pad; i++) uc_putc(' ');

  uc_putc(entry_suffix(e));
  uc_putc(' ');

  char sbuf[10];
  fmt_size(sbuf, e);
  emit(sbuf);

  if (is_cursor) emit(C_RST);
}

static void draw_stat_strip(int row, int col, int width,
                            const pile_pane_t *pane) {
  cursor_to(row, col);
  emit(C_FRAME);
  for (int i = 0; i < width; i++) uc_putc('-');
  emit(C_RST);

  cursor_to(row + 1, col);
  if (pane->count == 0) {
    emit(C_FRAME);
    emit(" (empty directory) ");
    emit(C_RST);
    clear_to_eol();
    return;
  }

  const pile_entry_t *e = &pane->entries[pane->cursor];
  /* Line: "  cursor/total   <name>" — file stat details come in P2. */
  uc_putc(' ');
  uc_putc(' ');
  put_uint(pane->cursor + 1);
  uc_putc('/');
  put_uint(pane->count);
  if (pane->truncated) emit("+");
  uc_putc(' ');
  uc_putc(' ');
  emit(entry_color(e));
  emit(e->name);
  emit(C_RST);
  clear_to_eol();
}

static void draw_legend(int row, int col, int width) {
  cursor_to(row, col);
  emit(C_FRAME);
  for (int i = 0; i < width; i++) uc_putc('-');
  emit(C_RST);

  cursor_to(row + 1, col);
  /* P1 legend: minimal — navigation + quit.  The richer F-key legend
   * lands with the file operations (P3) and viewer (P4). */
  emit(" ");
  emit(C_KEY); emit("ENTER"); emit(C_RST); emit(" open  ");
  emit(C_KEY); emit("BS"); emit(C_RST); emit(" parent  ");
  emit(C_KEY); emit("F10"); emit(C_RST); emit("/");
  emit(C_KEY); emit("q"); emit(C_RST); emit(" quit");
  clear_to_eol();
}

/* ── Public entrypoints ───────────────────────────────────────────────── */

void pile_draw_all(void) {
  cursor_hide();

  /* Row layout (single pane):
   *   0              path header
   *   1 .. vrows     entry list
   *   vrows+1        horizontal rule (part of stat_strip)
   *   vrows+2        stat line
   *   vrows+3        key legend (rule + legend line)
   *
   * visible_rows() in pile.c returns rows-4, i.e. the entry list
   * height.  We recompute here to keep this file self-contained. */
  int vrows = pile_rows - 4;
  if (vrows < 1) vrows = 1;

  draw_path_header(0, 0, pile_cols, pile_active);

  for (int i = 0; i < vrows; i++) {
    int idx = pile_active->scroll + i;
    int is_cur = (idx == pile_active->cursor) && idx < pile_active->count;
    draw_entry_row(1 + i, 0, pile_cols, pile_active, idx, is_cur);
    clear_to_eol();
  }

  /* Stat strip = 2 lines (rule + stats).  Legend = 1 line (rule-less
   * label; rule above is part of stat_strip).  Total chrome below the
   * list is 3 lines; we have 4 reserved (header + 3 below). */
  draw_stat_strip(1 + vrows, 0, pile_cols, pile_active);
  draw_legend(1 + vrows + 2, 0, pile_cols);

  /* Park the terminal cursor off-screen (bottom-right) so it doesn't
   * blink inside the listing. */
  cursor_to(pile_rows - 1, pile_cols - 1);
  cursor_show();
}

void pile_draw_clear(void) {
  attr_reset();
  clear_screen();
  cursor_to(0, 0);
  cursor_show();
}
