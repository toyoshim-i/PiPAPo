/*
 * pi_ui.c — Screen refresh for PiPAPo Editor
 *
 * Renders top-to-bottom each frame:
 *   Row 0:            Menu bar
 *   Rows 1..rows-3:   Content area (line numbers + text + tilde lines)
 *   Row rows-2:       Status bar
 *   Row rows-1:       Hint bar
 *   Overlay:          Dropdown menu (in MODE_MENU)
 */

#include "pi.h"

/* ── Helpers ───────────────────────────────────────────────────────────── */

/* Length-bounded write: fputs requires NUL termination, so this stays
 * local for the one caller that emits a slice of the row buffer. */
static void put_nstr(const char *s, int len) { write(1, s, len); }

/* Write integer (up to 5 digits, right-justified in `width` chars).
 * vsnprintf does not accept runtime widths (no "%*d"), and the
 * compile-time width here depends on the live gutter size, so the
 * formatting stays open-coded. */
static void put_int_rj(int val, int width) {
  char tmp[8];
  int i = 0;
  if (val == 0) {
    tmp[i++] = '0';
  } else {
    while (val > 0 && i < 7) {
      tmp[i++] = '0' + val % 10;
      val /= 10;
    }
  }
  for (int j = i; j < width; j++) putchar(' ');
  for (int j = i - 1; j >= 0; j--) putchar(tmp[j]);
}

/* Erase to end of line */
static void erase_eol(void) { fputs("\033[K", stdout); }

/* ── Scroll adjustment ─────────────────────────────────────────────────── */

static int content_rows(void) {
  /* rows - 3: menu bar (1) + status bar (1) + hint bar (1) */
  int cr = E.rows - 3;
  return cr > 1 ? cr : 1;
}

static void adjust_scroll(void) {
  int cr = content_rows();
  if (E.cy < E.scroll_row)
    E.scroll_row = E.cy;
  if (E.cy >= E.scroll_row + cr)
    E.scroll_row = E.cy - cr + 1;
  if (E.scroll_row < 0)
    E.scroll_row = 0;
}

/* ── Line number gutter width ──────────────────────────────────────────── */

static int gutter_width(void) {
  int total = gap_line_count(&E.buf);
  int w = 1;
  while (total >= 10) {
    total /= 10;
    w++;
  }
  if (w < 3) w = 3; /* minimum 3 digits */
  return w + 1;      /* +1 for the '|' separator */
}

/* ── Draw menu bar (row 0) ─────────────────────────────────────────────── */

static void draw_menu_bar(void) {
  term_cursor_to(0, 0);
  term_attr_bold();
  term_attr_fg(COL_WHITE);
  term_attr_bg(COL_BLUE);

  int col = 0;
  for (int i = 0; i < MENU_CAT_COUNT; i++) {
    /* Pad to menu position */
    while (col < pi_menus[i].col) {
      putchar(' ');
      col++;
    }
    /* Highlight selected category in menu mode */
    if (E.mode == MODE_MENU && E.menu_cat == i) {
      term_attr_reset();
      term_attr_bold();
      term_attr_bg(COL_WHITE);
      term_attr_fg(COL_BLACK);
    }
    fputs(pi_menus[i].label, stdout);
    col += strlen(pi_menus[i].label);
    if (E.mode == MODE_MENU && E.menu_cat == i) {
      term_attr_reset();
      term_attr_bold();
      term_attr_fg(COL_WHITE);
      term_attr_bg(COL_BLUE);
    }
  }
  /* Fill rest of line */
  while (col < E.cols) {
    putchar(' ');
    col++;
  }
  term_attr_reset();
}

/* ── Draw content area (rows 1..rows-3) ────────────────────────────────── */

static void draw_content(void) {
  int cr = content_rows();
  int gw = gutter_width();
  int total_lines = gap_line_count(&E.buf);
  char rowbuf[512];

  for (int i = 0; i < cr; i++) {
    int file_row = E.scroll_row + i;
    int screen_row = i + 1; /* row 0 is menu bar */
    term_cursor_to(screen_row, 0);

    if (file_row < total_lines) {
      /* Line number (dim cyan) */
      term_attr_dim();
      term_attr_fg(COL_CYAN);
      put_int_rj(file_row + 1, gw - 1);
      term_attr_reset();
      putchar('|');

      /* Row content */
      int dummy;
      int len = gap_get_row(&E.buf, file_row, rowbuf,
                            (int)sizeof(rowbuf), &dummy);
      int avail = E.cols - gw;
      if (len > avail) len = avail;
      if (len > 0)
        put_nstr(rowbuf, len);
      erase_eol();
    } else {
      /* Tilde line (dim blue) */
      term_attr_dim();
      term_attr_fg(COL_BLUE);
      putchar('~');
      term_attr_reset();
      erase_eol();
    }
  }
}

/* ── Draw dropdown menu (overlay on content) ───────────────────────────── */

static void draw_dropdown(void) {
  if (E.mode != MODE_MENU)
    return;

  const menu_cat_t *cat = &pi_menus[E.menu_cat];
  int start_col = cat->col;

  /* Compute dropdown width: max(label + shortcut + padding) */
  int max_w = 0;
  for (int i = 0; i < cat->count; i++) {
    int w = strlen(cat->items[i].label);
    if (cat->items[i].shortcut[0])
      w += 2 + strlen(cat->items[i].shortcut); /* "  shortcut" */
    if (w > max_w) max_w = w;
  }
  int box_w = max_w + 4; /* 2 padding each side */

  /* Clamp to screen */
  if (start_col + box_w > E.cols)
    start_col = E.cols - box_w;
  if (start_col < 0) start_col = 0;

  /* Top border */
  term_cursor_to(1, start_col);
  term_attr_reset();
  putchar('+');
  for (int j = 0; j < box_w - 2; j++)
    putchar('-');
  putchar('+');

  /* Items */
  for (int i = 0; i < cat->count; i++) {
    int screen_row = 2 + i;

    term_cursor_to(screen_row, start_col);
    term_attr_reset();
    putchar('|');

    /* Highlight selected item */
    if (i == E.menu_item) {
      term_attr_reverse();
      term_attr_bold();
    }

    putchar(' ');
    fputs(cat->items[i].label, stdout);

    /* Right-align shortcut */
    int label_len = strlen(cat->items[i].label);
    int sc_len = strlen(cat->items[i].shortcut);
    int pad = box_w - 4 - label_len - sc_len;
    for (int j = 0; j < pad; j++)
      putchar(' ');

    if (sc_len > 0) {
      if (i != E.menu_item) {
        term_attr_dim();
      }
      fputs(cat->items[i].shortcut, stdout);
    }

    term_attr_reset();
    putchar(' ');
    putchar('|');
  }

  /* Bottom border */
  term_cursor_to(2 + cat->count, start_col);
  term_attr_reset();
  putchar('+');
  for (int j = 0; j < box_w - 2; j++)
    putchar('-');
  putchar('+');
}

/* ── Draw status bar (row rows-2) ──────────────────────────────────────── */

static void draw_status_bar(void) {
  term_cursor_to(E.rows - 2, 0);

  /* Color depends on dirty flag */
  term_attr_reverse();
  if (E.dirty)
    term_attr_fg(COL_YELLOW);
  else
    term_attr_fg(COL_GREEN);

  /* Left side: "-- pi: filename [+] -- MODE --" */
  fputs(" pi: ", stdout);
  if (E.filename[0])
    fputs(E.filename, stdout);
  else
    fputs("[new]", stdout);

  if (E.dirty)
    fputs(" [+]", stdout);

  fputs(" - ", stdout);

  /* Mode */
  switch (E.mode) {
  case MODE_NORMAL:  fputs("NORMAL", stdout);  break;
  case MODE_INSERT:  fputs("INSERT", stdout);  break;
  case MODE_MENU:    fputs("MENU", stdout);    break;
  case MODE_COMMAND: fputs("COMMAND", stdout); break;
  }

  fputs(" - ", stdout);

  /* Cursor position */
  printf("%d:%d", (int32_t)(E.cy + 1), (int32_t)(E.cx + 1));

  /* Status message (if any) */
  if (E.status[0]) {
    fputs(" | ", stdout);
    fputs(E.status, stdout);
  }

  /* Fill rest */
  erase_eol();
  term_attr_reset();
}

/* ── Draw hint bar (row rows-1) ────────────────────────────────────────── */

static void draw_hint_bar(void) {
  term_cursor_to(E.rows - 1, 0);
  term_attr_dim();

  switch (E.mode) {
  case MODE_NORMAL:
    fputs(" Esc Menu | i insert | :w save | :q quit | / search", stdout);
    break;
  case MODE_INSERT:
    fputs(" Esc normal | type to edit", stdout);
    break;
  case MODE_MENU:
    fputs(" <> category | ^v select | Enter run | Esc close", stdout);
    break;
  case MODE_COMMAND:
    fputs(" Enter execute | Esc cancel", stdout);
    break;
  }

  erase_eol();
  term_attr_reset();
}

/* ── Draw command line (replaces hint bar in command mode) ──────────────── */

static void draw_command_line(void) {
  if (E.mode != MODE_COMMAND)
    return;

  term_cursor_to(E.rows - 1, 0);
  term_attr_reset();
  putchar(':');
  if (E.cmd_len > 0)
    put_nstr(E.cmd, E.cmd_len);
  erase_eol();
}

/* ── Draw help screen (MODE_HELP) ──────────────────────────────────────── */

static const char *help_lines[] = {
    "",
    "  pi -- PiPAPo Editor",
    "",
    "  NORMAL MODE",
    "    h/j/k/l  or arrows   Move cursor",
    "    0  $                  Begin / end of line",
    "    w  b                  Next / previous word",
    "    gg  G                 Top / bottom of file",
    "    i                     Insert before cursor",
    "    a                     Append after cursor",
    "    o  O                  Open line below / above",
    "    x                     Delete character",
    "    dd                    Delete line",
    "    u                     Undo last edit",
    "    /pattern              Search forward",
    "    n  N                  Next / previous match",
    "    :w                    Save",
    "    :q                    Quit  (:q! force)",
    "    :wq                   Save and quit",
    "    :e file               Open file",
    "    :N                    Go to line N",
    "    Esc                   Open menu",
    "",
    "  INSERT MODE",
    "    Type to edit.  Esc returns to normal mode.",
    "",
    "  MENU MODE",
    "    Arrow keys navigate.  Enter selects.  Esc closes.",
    "",
    "              Press any key to return",
    (void *)0,
};

static void draw_help(void) {
  int cr = content_rows();
  for (int i = 0; i < cr; i++) {
    term_cursor_to(i + 1, 0);
    if (help_lines[i]) {
      term_attr_reset();
      if (i == 1) {
        /* Title line: bold cyan */
        term_attr_bold();
        term_attr_fg(COL_CYAN);
      } else if (i == 3 || i == 24 || i == 27) {
        /* Section headers: bold */
        term_attr_bold();
      }
      fputs(help_lines[i], stdout);
      term_attr_reset();
    }
    erase_eol();
  }
}

/* ── Public: full screen refresh ───────────────────────────────────────── */

void ui_set_status(const char *msg) {
  int i = 0;
  while (msg[i] && i < (int)sizeof(E.status) - 1) {
    E.status[i] = msg[i];
    i++;
  }
  E.status[i] = '\0';
}

void ui_refresh(void) {
  adjust_scroll();
  term_cursor_hide();

  draw_menu_bar();

  if (E.mode == MODE_HELP) {
    draw_help();
    draw_status_bar();
    term_cursor_to(E.rows - 1, 0);
    term_attr_dim();
    fputs(" Press any key to return", stdout);
    erase_eol();
    term_attr_reset();
    term_cursor_show();
    return;
  }

  draw_content();
  draw_status_bar();

  if (E.mode == MODE_COMMAND)
    draw_command_line();
  else
    draw_hint_bar();

  /* Dropdown overlay (drawn last so it covers content) */
  draw_dropdown();

  /* Position cursor */
  if (E.mode == MODE_COMMAND) {
    /* Cursor on command line */
    term_cursor_to(E.rows - 1, 1 + E.cmd_len);
  } else if (E.mode != MODE_MENU) {
    /* Cursor in content area */
    int gw = gutter_width();
    int screen_row = (E.cy - E.scroll_row) + 1;
    int screen_col = gw + E.cx;
    term_cursor_to(screen_row, screen_col);
  }

  term_cursor_show();
}
