/*
 * pi_buf.c — Gap buffer for PiPAPo Editor
 *
 * O(1) insert/delete at cursor.  Memory obtained via brk().
 */

#include "pi.h"

#define GAP_GROW 1024

/* ── Helpers ───────────────────────────────────────────────────────────── */

static void gap_grow(gap_buf_t *g, int need) {
  int text_len = gap_length(g);
  int new_cap = g->cap;
  while (new_cap - text_len < need)
    new_cap += GAP_GROW;
  if (new_cap == g->cap)
    return;

  /* Expand via brk */
  char *new_data = brk((void *)0);
  new_data = brk(new_data + new_cap);
  if (!new_data)
    return;
  new_data = g->data; /* brk extends in place */

  /* Shift post-gap data to the end of new buffer */
  int post_len = g->cap - g->gap_end;
  int new_gap_end = new_cap - post_len;
  /* Move from old end to new end (may overlap, copy backwards) */
  for (int i = post_len - 1; i >= 0; i--)
    new_data[new_gap_end + i] = new_data[g->gap_end + i];

  g->data = new_data;
  g->gap_end = new_gap_end;
  g->cap = new_cap;
}

/* ── Public API ────────────────────────────────────────────────────────── */

void gap_init(gap_buf_t *g, int initial_cap) {
  if (initial_cap < GAP_GROW)
    initial_cap = GAP_GROW;

  /* Allocate via brk */
  char *base = brk((void *)0);
  char *end = brk(base + initial_cap);
  if (!end) {
    g->data = (void *)0;
    g->cap = 0;
    g->gap_start = 0;
    g->gap_end = 0;
    return;
  }
  g->data = base;
  g->cap = initial_cap;
  g->gap_start = 0;
  g->gap_end = initial_cap;
}

void gap_insert(gap_buf_t *g, char c) {
  if (g->gap_start >= g->gap_end)
    gap_grow(g, 1);
  g->data[g->gap_start++] = c;
}

void gap_delete(gap_buf_t *g) {
  if (g->gap_start > 0)
    g->gap_start--;
}

void gap_delete_fwd(gap_buf_t *g) {
  if (g->gap_end < g->cap)
    g->gap_end++;
}

void gap_delete_line(gap_buf_t *g) {
  /* Move to beginning of current line */
  while (g->gap_start > 0 && g->data[g->gap_start - 1] != '\n')
    g->gap_start--;

  /* Delete forward to end of line (including newline) */
  while (g->gap_end < g->cap && g->data[g->gap_end] != '\n')
    g->gap_end++;
  /* Consume the trailing newline if present */
  if (g->gap_end < g->cap && g->data[g->gap_end] == '\n')
    g->gap_end++;
}

void gap_move(gap_buf_t *g, int pos) {
  int text_len = gap_length(g);
  if (pos < 0) pos = 0;
  if (pos > text_len) pos = text_len;

  while (g->gap_start > pos) {
    g->gap_end--;
    g->gap_start--;
    g->data[g->gap_end] = g->data[g->gap_start];
  }
  while (g->gap_start < pos) {
    g->data[g->gap_start] = g->data[g->gap_end];
    g->gap_start++;
    g->gap_end++;
  }
}

int gap_length(gap_buf_t *g) {
  return g->cap - (g->gap_end - g->gap_start);
}

int gap_line_count(gap_buf_t *g) {
  int count = 1;
  int len = gap_length(g);
  for (int i = 0; i < len; i++) {
    char c;
    if (i < g->gap_start)
      c = g->data[i];
    else
      c = g->data[g->gap_end + (i - g->gap_start)];
    if (c == '\n')
      count++;
  }
  return count;
}

int gap_char_at(gap_buf_t *g, int pos) {
  if (pos < 0 || pos >= gap_length(g))
    return -1;
  if (pos < g->gap_start)
    return (unsigned char)g->data[pos];
  return (unsigned char)g->data[g->gap_end + (pos - g->gap_start)];
}

int gap_get_row(gap_buf_t *g, int row, char *buf, int bufsize, int *row_start) {
  int text_len = gap_length(g);
  int cur_row = 0;
  int pos = 0;

  /* Find start of requested row */
  while (cur_row < row && pos < text_len) {
    int c = gap_char_at(g, pos);
    pos++;
    if (c == '\n')
      cur_row++;
  }

  if (row_start)
    *row_start = pos;

  /* Copy row content */
  int len = 0;
  while (pos < text_len && len < bufsize - 1) {
    int c = gap_char_at(g, pos);
    if (c == '\n')
      break;
    buf[len++] = (char)c;
    pos++;
  }
  buf[len] = '\0';
  return len;
}

int gap_pos_from_rc(gap_buf_t *g, int row, int col) {
  int text_len = gap_length(g);
  int cur_row = 0;
  int pos = 0;

  /* Find start of row */
  while (cur_row < row && pos < text_len) {
    if (gap_char_at(g, pos) == '\n')
      cur_row++;
    pos++;
  }

  /* Advance by col, but don't go past end of line */
  int c = 0;
  while (c < col && pos < text_len) {
    if (gap_char_at(g, pos) == '\n')
      break;
    pos++;
    c++;
  }
  return pos;
}

void gap_rc_from_pos(gap_buf_t *g, int pos, int *row, int *col) {
  int r = 0, c = 0;
  int len = gap_length(g);
  if (pos > len) pos = len;
  for (int i = 0; i < pos; i++) {
    if (gap_char_at(g, i) == '\n') {
      r++;
      c = 0;
    } else {
      c++;
    }
  }
  *row = r;
  *col = c;
}

int gap_row_len(gap_buf_t *g, int row) {
  int dummy;
  char tmp[1];
  /* We just need the length — use get_row logic but don't copy much */
  int text_len = gap_length(g);
  int cur_row = 0;
  int pos = 0;
  (void)tmp;
  (void)dummy;

  while (cur_row < row && pos < text_len) {
    if (gap_char_at(g, pos) == '\n')
      cur_row++;
    pos++;
  }

  int len = 0;
  while (pos < text_len) {
    if (gap_char_at(g, pos) == '\n')
      break;
    len++;
    pos++;
  }
  return len;
}
