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

static void put_char(char c) { write(1, &c, 1); }

static void put_str(const char *s) {
  int n = 0;
  while (s[n]) n++;
  write(1, s, n);
}

static void put_nstr(const char *s, int len) { write(1, s, len); }

/* Write integer (up to 5 digits, right-justified in `width` chars) */
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
  /* Pad with spaces */
  for (int j = i; j < width; j++)
    put_char(' ');
  /* Print digits in reverse */
  for (int j = i - 1; j >= 0; j--)
    put_char(tmp[j]);
}

/* Erase to end of line */
static void erase_eol(void) { put_str("\033[K"); }

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
      put_char(' ');
      col++;
    }
    /* Highlight selected category in menu mode */
    if (E.mode == MODE_MENU && E.menu_cat == i) {
      term_attr_reset();
      term_attr_bold();
      term_attr_bg(COL_WHITE);
      term_attr_fg(COL_BLACK);
    }
    put_str(pi_menus[i].label);
    col += uc_strlen(pi_menus[i].label);
    if (E.mode == MODE_MENU && E.menu_cat == i) {
      term_attr_reset();
      term_attr_bold();
      term_attr_fg(COL_WHITE);
      term_attr_bg(COL_BLUE);
    }
  }
  /* Fill rest of line */
  while (col < E.cols) {
    put_char(' ');
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
      put_char('|');

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
      put_char('~');
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
    int w = uc_strlen(cat->items[i].label);
    if (cat->items[i].shortcut[0])
      w += 2 + uc_strlen(cat->items[i].shortcut); /* "  shortcut" */
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
  put_char('+');
  for (int j = 0; j < box_w - 2; j++)
    put_char('-');
  put_char('+');

  /* Items */
  for (int i = 0; i < cat->count; i++) {
    int screen_row = 2 + i;

    term_cursor_to(screen_row, start_col);
    term_attr_reset();
    put_char('|');

    /* Highlight selected item */
    if (i == E.menu_item) {
      term_attr_reverse();
      term_attr_bold();
    }

    put_char(' ');
    put_str(cat->items[i].label);

    /* Right-align shortcut */
    int label_len = uc_strlen(cat->items[i].label);
    int sc_len = uc_strlen(cat->items[i].shortcut);
    int pad = box_w - 4 - label_len - sc_len;
    for (int j = 0; j < pad; j++)
      put_char(' ');

    if (sc_len > 0) {
      if (i != E.menu_item) {
        term_attr_dim();
      }
      put_str(cat->items[i].shortcut);
    }

    term_attr_reset();
    put_char(' ');
    put_char('|');
  }

  /* Bottom border */
  term_cursor_to(2 + cat->count, start_col);
  term_attr_reset();
  put_char('+');
  for (int j = 0; j < box_w - 2; j++)
    put_char('-');
  put_char('+');
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
  put_str(" pi: ");
  if (E.filename[0])
    put_str(E.filename);
  else
    put_str("[new]");

  if (E.dirty)
    put_str(" [+]");

  put_str(" - ");

  /* Mode */
  switch (E.mode) {
  case MODE_NORMAL:  put_str("NORMAL");  break;
  case MODE_INSERT:  put_str("INSERT");  break;
  case MODE_MENU:    put_str("MENU");    break;
  case MODE_COMMAND: put_str("COMMAND"); break;
  }

  put_str(" - ");

  /* Cursor position */
  char pos_buf[24];
  uc_snprintf(pos_buf, sizeof(pos_buf), "%d:%d", (int32_t)(E.cy + 1),
              (int32_t)(E.cx + 1));
  put_str(pos_buf);

  /* Status message (if any) */
  if (E.status[0]) {
    put_str(" | ");
    put_str(E.status);
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
    put_str(" Esc Menu | i insert | :w save | :q quit | / search");
    break;
  case MODE_INSERT:
    put_str(" Esc normal | type to edit");
    break;
  case MODE_MENU:
    put_str(" <> category | ^v select | Enter run | Esc close");
    break;
  case MODE_COMMAND:
    put_str(" Enter execute | Esc cancel");
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
  put_char(':');
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
      put_str(help_lines[i]);
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
    put_str(" Press any key to return");
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
