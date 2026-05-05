/*
 * pile_draw.c — VT100 rendering for the pile filer
 *
 * User guide: docs/user/pile.md
 *
 * Chrome layout (identical for single-pane and two-pane modes, 6 rows):
 *   row 0                   pane header(s)
 *   rows 1..vrows           entry list
 *   row vrows + 1           per-pane footer (cursor/count, size total)
 *   row vrows + 2           horizontal rule
 *   row vrows + 3           stat strip line 1: full file path
 *   row vrows + 4           stat strip line 2: size / mode / mtime
 *   row vrows + 5           bottom bar: rule + "?: help" hint, or
 *                           transient status overlay, or prompt line
 *
 * visible_rows = pile_rows - 6.  The vertical divider in two-pane
 * mode spans rows 0 .. vrows+1 (header through per-pane footer); the
 * stat strip spans the full width below it.  The '?' key brings up
 * a full-screen help overlay listing all key bindings; any key
 * dismisses it.
 */

#include "pile.h"

#define C(seq) (pile_use_color ? (seq) : "")
#define C_RST      C("\033[0m")
#define C_DIR      C("\033[1;34m")
#define C_EXEC     C("\033[1;32m")
#define C_LINK     C("\033[36m")
#define C_DEV      C("\033[1;33m")
/* Active pane shares a cyan-background look between its header and
 * its cursor line — consistent "this is the active side" signal.
 * Inactive cursor stays plain reverse (visible but clearly secondary);
 * inactive header is dim.  C_MARK keeps plain reverse so a marked
 * entry on the inactive pane is distinguishable from a cursored one
 * by the leading '*' glyph we already emit. */
#define C_CUR      C("\033[1;37;46m")  /* active: bold white on cyan */
#define C_CUR_OFF  C("\033[7m")        /* inactive: plain reverse */
#define C_MARK     C("\033[7m")
#define C_FRAME    C("\033[2m")
#define C_HEADER   C("\033[1;37;46m")  /* active header: bold white on cyan */
#define C_HEADER_OFF C("\033[2m")      /* inactive header: dim */
#define C_KEY      C("\033[1m")
#define C_WARN     C("\033[33m")
#define C_ERR      C("\033[31m")

#define CHROME_ROWS 6

/* ── Low-level ANSI helpers ───────────────────────────────────────────── */

void pile_draw_cursor_to(int row, int col) {
  printf("\033[%d;%dH", (int32_t)(row + 1), (int32_t)(col + 1));
}

void pile_draw_clear_to_eol(void) { fputs("\033[K", stdout); }

static void clear_screen(void) { fputs("\033[2J\033[H", stdout); }
static void cursor_hide(void) { fputs("\033[?25l", stdout); }
static void cursor_show(void) { fputs("\033[?25h", stdout); }
static void attr_reset(void) { fputs("\033[0m", stdout); }

static void put_spaces(int n) {
  for (int i = 0; i < n; i++) putchar(' ');
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
  int slen = strlen(suffix);
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
  pile_draw_cursor_to(row, col);
  fputs(is_active ? C_HEADER : C_HEADER_OFF, stdout);
  fputs("-- ", stdout);
  int used = 3;
  int plen = strlen(pane->path);
  int max_path = width - 5;
  if (max_path < 4) max_path = 4;
  if (plen > max_path) {
    /* Truncate the head of the path — keep the tail visible. */
    putchar('~');
    used++;
    const char *tail = pane->path + (plen - (max_path - 1));
    int tail_len = max_path - 1;
    for (int i = 0; i < tail_len; i++) putchar(tail[i]);
    used += tail_len;
  } else {
    fputs(pane->path, stdout);
    used += plen;
  }
  putchar(' ');
  used++;
  fputs(C_RST, stdout);
  fputs(C_FRAME, stdout);
  while (used < width) { putchar('-'); used++; }
  fputs(C_RST, stdout);
}

static void draw_entry_row(int row, int col, int width,
                           const pile_pane_t *pane, int idx,
                           int is_cursor_active, int is_cursor_inactive) {
  pile_draw_cursor_to(row, col);
  if (idx < 0 || idx >= pane->count) {
    put_spaces(width);
    return;
  }
  const pile_entry_t *e = &pane->entries[idx];
  int is_marked = (e->flags & PILE_EFLAG_MARKED) != 0;

  /* Cursor wins over mark visually; both use reverse video and the
   * gutter glyph disambiguates.  Color scopes the entire row so the
   * whole line reads as highlighted. */
  if (is_cursor_active) fputs(C_CUR, stdout);
  else if (is_cursor_inactive) fputs(C_CUR_OFF, stdout);
  else if (is_marked) fputs(C_MARK, stdout);

  putchar(is_marked ? '*' : ' ');

  /* name | suffix | space | 9-col size */
  int name_col = width - 1 - 1 - 1 - 9;
  if (name_col < 4) name_col = 4;

  int scoped = is_cursor_active || is_cursor_inactive || is_marked;
  if (!scoped) fputs(entry_color(e), stdout);
  int nlen = strlen(e->name);
  int shown = nlen < name_col ? nlen : name_col;
  for (int i = 0; i < shown; i++) putchar(e->name[i]);
  if (!scoped) fputs(C_RST, stdout);

  int pad = name_col - shown;
  put_spaces(pad);
  putchar(entry_suffix(e));
  putchar(' ');

  char sbuf[10];
  fmt_size(sbuf, e);
  fputs(sbuf, stdout);

  if (scoped) fputs(C_RST, stdout);
}

/* Pane footer row: "  i/n  [N sel]                    total" where
 * the sel count is omitted when nothing is marked.  Total is the sum
 * of regular-file sizes (marked subset if any entries are marked;
 * whole pane otherwise). */
static void draw_pane_footer(int row, int col, int width,
                             const pile_pane_t *pane) {
  pile_draw_cursor_to(row, col);
  fputs(C_FRAME, stdout);
  put_spaces(2);
  printf("%u", (uint32_t)(pane->cursor + (pane->count ? 1 : 0)));
  putchar('/');
  printf("%u", (uint32_t)(pane->count));
  if (pane->truncated) putchar('+');

  int sel = pile_pane_sel_count(pane);
  if (sel > 0) {
    fputs("  ", stdout);
    fputs(C_RST, stdout);
    printf("%u", (uint32_t)(sel));
    fputs(C_FRAME, stdout);
    fputs(" sel", stdout);
  }
  fputs(C_RST, stdout);

  uint32_t total = 0;
  for (int i = 0; i < pane->count; i++) {
    const pile_entry_t *e = &pane->entries[i];
    if (e->d_type != DT_REG) continue;
    if (sel > 0 && !(e->flags & PILE_EFLAG_MARKED)) continue;
    total += e->size;
  }
  char sz[8];
  fmt_size_compact(sz, total);
  int slen = strlen(sz);
  int total_col = col + width - 1 - slen;
  pile_draw_cursor_to(row, total_col);
  fputs(C_FRAME, stdout);
  fputs(sz, stdout);
  fputs(C_RST, stdout);
}

/* ── Divider column ───────────────────────────────────────────────────── */

static void draw_divider(int col, int first_row, int last_row) {
  fputs(C_FRAME, stdout);
  for (int r = first_row; r <= last_row; r++) {
    pile_draw_cursor_to(r, col);
    putchar('|');
  }
  fputs(C_RST, stdout);
}

/* ── Global strips ────────────────────────────────────────────────────── */

static void draw_rule(int row) {
  pile_draw_cursor_to(row, 0);
  fputs(C_FRAME, stdout);
  for (int i = 0; i < pile_cols; i++) putchar('-');
  fputs(C_RST, stdout);
}

static void draw_stat_strip(int row_path, int row_detail) {
  const pile_pane_t *pane = pile_active;

  pile_draw_cursor_to(row_path, 0);
  if (pane->count == 0) {
    fputs(C_FRAME, stdout);
    fputs(" (empty directory)", stdout);
    fputs(C_RST, stdout);
    pile_draw_clear_to_eol();
    pile_draw_cursor_to(row_detail, 0);
    pile_draw_clear_to_eol();
    return;
  }

  const pile_entry_t *e = &pane->entries[pane->cursor];

  /* Line 1: leading "  " + pane path + "/" + name */
  putchar(' ');
  putchar(' ');
  fputs(pane->path, stdout);
  int plen = strlen(pane->path);
  if (plen == 0 || pane->path[plen - 1] != '/') putchar('/');
  fputs(entry_color(e), stdout);
  fputs(e->name, stdout);
  fputs(C_RST, stdout);
  pile_draw_clear_to_eol();

  /* Line 2: size + mode + mtime (if available) */
  pile_draw_cursor_to(row_detail, 0);
  putchar(' ');
  putchar(' ');
  char sbuf[10];
  fmt_size(sbuf, e);
  /* trim leading spaces from the right-justified size */
  int sp = 0;
  while (sbuf[sp] == ' ') sp++;
  fputs(sbuf + sp, stdout);
  fputs("  ", stdout);
  char mode_buf[11];
  fmt_mode(mode_buf, e->mode, e->d_type);
  fputs(mode_buf, stdout);
  if (e->mtime) {
    fputs("  ", stdout);
    char tbuf[17];
    uc_format_ymdhm(tbuf, e->mtime);
    fputs(C_FRAME, stdout);
    fputs(tbuf, stdout);
    fputs(C_RST, stdout);
  }
  pile_draw_clear_to_eol();
}

static void draw_status(int row) {
  pile_draw_cursor_to(row, 0);
  putchar(' ');
  fputs(pile_status_is_error ? C_ERR : C_WARN, stdout);
  fputs(pile_status_msg, stdout);
  fputs(C_RST, stdout);
  pile_draw_clear_to_eol();
}

/* Bottom bar: horizontal rule with a "?: help" hint flush-right.
 * Always leaves the last column empty — writing to the bottom-right
 * corner sets the pending-wrap latch on many VT emulators, and the
 * next output then scrolls the screen by one row. */
static void draw_bottom_bar(int row) {
  pile_draw_cursor_to(row, 0);
  fputs(C_FRAME, stdout);
  int usable = pile_cols - 1;  /* reserve the last column */
  if (usable < 11) {
    for (int i = 0; i < usable; i++) putchar('-');
    fputs(C_RST, stdout);
    return;
  }
  const int hint_width = 8;  /* " ?: help" */
  int dashes = usable - hint_width;
  for (int i = 0; i < dashes; i++) putchar('-');
  putchar(' ');
  fputs(C_RST, stdout);
  fputs(C_KEY, stdout); putchar('?'); fputs(C_RST, stdout);
  fputs(C_FRAME, stdout);
  fputs(": help", stdout);
  fputs(C_RST, stdout);
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
  pile_draw_cursor_to(1 + vrows, col);
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
  if (pile_status_msg[0]) {
    draw_status(1 + vrows + 4);
  } else {
    draw_bottom_bar(1 + vrows + 4);
  }

  /* Park the cursor on the bottom bar away from the rightmost
   * column.  Writing or positioning at (rows-1, cols-1) sets the
   * "pending wrap" latch on many VT emulators; any subsequent
   * output then scrolls the screen by one row. */
  pile_draw_cursor_to(pile_rows - 1, 0);
  cursor_show();
}

void pile_draw_clear(void) {
  attr_reset();
  clear_screen();
  pile_draw_cursor_to(0, 0);
  cursor_show();
}

/* ── Help overlay ─────────────────────────────────────────────────────── */

/* Full-screen key-binding reference.  Caller is expected to
 * pile_read_key() afterwards to wait for dismissal; the main loop's
 * next iteration then repaints pile.
 *
 * Condensed into ~16 rows by grouping related bindings onto one
 * line, so the help fits inside a 20-row terminal with no scroll. */
void pile_show_help(void) {
  cursor_hide();
  clear_screen();

  struct entry { const char *left; const char *right; };
  static const struct entry body[] = {
    /* { "", 0 } = blank spacer; right==0 = group header */
    { "Navigation", 0 },
    { "  up / down  PgUp / PgDn",      "scroll" },
    { "  Home / End",                  "top / bottom" },
    { "  TAB",                         "switch active pane" },
    { "  left / right",                "pane switch / parent dir" },
    { "  ENTER",                       "open dir or view file" },
    { "  BS",                          "parent dir" },
    { "", 0 },
    { "Marking", 0 },
    { "  SPACE",                       "toggle mark on cursor" },
    { "  + / -  *",                    "mark/unmark by glob, invert" },
    { "", 0 },
    { "Actions", 0 },
    { "  v / V",                       "view (auto / force-hex)" },
    { "  e",                           "edit (spawns /bin/pi)" },
    { "  c / m / d",                   "copy / move / delete" },
    { "  k",                           "mkdir" },
    { "", 0 },
    { "  s  /  .",                     "cycle sort / toggle hidden" },
    { "", 0 },
    { "Other", 0 },
    { "  !",                           "spawn /bin/sh" },
    { "  Ctrl-L",                      "redraw / re-query winsize" },
    { "  q  (Ctrl-Q)",                 "quit" },
  };
  int n = (int)(sizeof(body) / sizeof(body[0]));

  /* Title on row 0 */
  pile_draw_cursor_to(0, 2);
  fputs(C_HEADER, stdout);
  fputs("pile -- key bindings", stdout);
  fputs(C_RST, stdout);

  /* Body rows 2 .. pile_rows - 2 (inclusive).  The body->right=0
   * entries are group headers (bold); empty left marks a spacer. */
  int body_top = 2;
  int body_bot = pile_rows - 2;  /* leave row pile_rows-1 for dismissal */
  int row = body_top;
  int pad_to = pile_cols >= 50 ? 32 : 22;
  for (int i = 0; i < n && row <= body_bot; i++, row++) {
    pile_draw_cursor_to(row, 0);
    if (!body[i].left[0]) continue;  /* blank spacer */
    if (!body[i].right) {
      fputs(C_KEY, stdout);
      putchar(' ');
      fputs(body[i].left, stdout);
      fputs(C_RST, stdout);
    } else {
      fputs(body[i].left, stdout);
      int left_len = strlen(body[i].left);
      while (left_len < pad_to) { putchar(' '); left_len++; }
      fputs(C_FRAME, stdout);
      fputs(body[i].right, stdout);
      fputs(C_RST, stdout);
    }
  }

  /* Dismissal hint sits immediately after the body so there is no
   * empty band between the two.  If body would overflow, fall back
   * to pile_rows - 1 (overwriting the final body row). */
  int dismiss_row = row;
  if (dismiss_row > pile_rows - 1) dismiss_row = pile_rows - 1;
  pile_draw_cursor_to(dismiss_row, 0);
  fputs(C_FRAME, stdout);
  fputs(" any key to dismiss ", stdout);
  fputs(C_RST, stdout);
  pile_draw_clear_to_eol();
}
