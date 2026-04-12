/*
 * pi.c — PiPAPo Editor main loop
 *
 * Dispatches input by mode: normal, insert, menu, command.
 * E-5: normal mode navigation + menu mode.
 */

#include "pi.h"
#include "common/fcntl.h"

/* Global editor state */
editor_t E;

/* ── File loading ──────────────────────────────────────────────────────── */

void load_file(const char *path) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    /* New file — just set filename */
    uc_strncpy(E.filename, path, sizeof(E.filename));
    ui_set_status("(new file)");
    return;
  }

  uc_strncpy(E.filename, path, sizeof(E.filename));

  char buf[512];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0)
      break;
    for (int i = 0; i < n; i++)
      gap_insert(&E.buf, buf[i]);
  }
  close(fd);

  /* Move cursor back to start */
  gap_move(&E.buf, 0);
  E.cx = 0;
  E.cy = 0;
  E.dirty = 0;
}

/* ── File saving ───────────────────────────────────────────────────────── */

int save_file(const char *path) {
  if (!path || !path[0]) {
    ui_set_status("No filename");
    return -1;
  }

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    ui_set_status("Error: cannot open for writing");
    return -1;
  }

  int len = gap_length(&E.buf);
  char buf[512];
  int written = 0;
  int bi = 0;

  for (int i = 0; i < len; i++) {
    buf[bi++] = (char)gap_char_at(&E.buf, i);
    if (bi == (int)sizeof(buf)) {
      ssize_t n = write(fd, buf, bi);
      if (n < 0) {
        close(fd);
        ui_set_status("Error: write failed");
        return -1;
      }
      written += n;
      bi = 0;
    }
  }
  if (bi > 0) {
    ssize_t n = write(fd, buf, bi);
    if (n < 0) {
      close(fd);
      ui_set_status("Error: write failed");
      return -1;
    }
    written += n;
  }
  close(fd);

  uc_strncpy(E.filename, path, sizeof(E.filename));
  E.dirty = 0;

  char msg[80];
  uc_snprintf(msg, sizeof(msg), "Written %d bytes", (int32_t)written);
  ui_set_status(msg);
  return 0;
}

/* Reset buffer for :e! (new) or :e file (open) */
void reset_buffer(void) {
  gap_move(&E.buf, 0);
  /* Delete everything */
  while (E.buf.gap_end < E.buf.cap)
    E.buf.gap_end++;
  E.cx = 0;
  E.cy = 0;
  E.scroll_row = 0;
  E.dirty = 0;
  E.filename[0] = '\0';
}

/* ── Undo snapshot ─────────────────────────────────────────────────────── */

static void take_snapshot(void) {
  gap_snapshot(&E.buf, &E.undo_snap, &E.undo_len);
  E.undo_cx = E.cx;
  E.undo_cy = E.cy;
}

/* ── Search helpers ────────────────────────────────────────────────────── */

void search_next(void) {
  if (E.search_len == 0) {
    ui_set_status("No search term");
    return;
  }
  int cur_pos = gap_pos_from_rc(&E.buf, E.cy, E.cx);
  int pos = gap_search_fwd(&E.buf, E.search, E.search_len, cur_pos + 1);
  /* Wrap around */
  if (pos < 0)
    pos = gap_search_fwd(&E.buf, E.search, E.search_len, 0);
  if (pos >= 0) {
    gap_rc_from_pos(&E.buf, pos, &E.cy, &E.cx);
    ui_set_status("");
  } else {
    ui_set_status("Pattern not found");
  }
}

void search_prev(void) {
  if (E.search_len == 0) {
    ui_set_status("No search term");
    return;
  }
  int cur_pos = gap_pos_from_rc(&E.buf, E.cy, E.cx);
  int pos = gap_search_bwd(&E.buf, E.search, E.search_len, cur_pos - 1);
  /* Wrap around */
  if (pos < 0)
    pos = gap_search_bwd(&E.buf, E.search, E.search_len,
                         gap_length(&E.buf) - 1);
  if (pos >= 0) {
    gap_rc_from_pos(&E.buf, pos, &E.cy, &E.cx);
    ui_set_status("");
  } else {
    ui_set_status("Pattern not found");
  }
}

/* ── Cursor helpers ────────────────────────────────────────────────────── */

static void clamp_cx(void) {
  int rlen = gap_row_len(&E.buf, E.cy);
  /* In normal mode, cursor can't go past last char (unless empty line) */
  if (E.mode == MODE_NORMAL && rlen > 0 && E.cx >= rlen)
    E.cx = rlen - 1;
  if (E.cx < 0)
    E.cx = 0;
}

static int is_word_char(int c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

/* ── Normal mode ───────────────────────────────────────────────────────── */

static void handle_normal(int key) {
  int total_lines = gap_line_count(&E.buf);

  /* Pending gg */
  if (E.pending_g) {
    E.pending_g = 0;
    if (key == 'g') {
      E.cy = 0;
      E.cx = 0;
      return;
    }
    /* Not 'g' — fall through to normal handling */
  }

  /* Pending dd */
  if (E.pending_d) {
    E.pending_d = 0;
    if (key == 'd') {
      take_snapshot();
      gap_move(&E.buf, gap_pos_from_rc(&E.buf, E.cy, 0));
      gap_delete_line(&E.buf);
      E.cx = 0;
      E.dirty = 1;
      /* Clamp cy if we deleted the last line */
      total_lines = gap_line_count(&E.buf);
      if (E.cy >= total_lines && total_lines > 0)
        E.cy = total_lines - 1;
      clamp_cx();
      return;
    }
  }

  switch (key) {
  /* ── Movement ── */
  case 'h':
  case KEY_LEFT:
    if (E.cx > 0) E.cx--;
    break;

  case 'l':
  case KEY_RIGHT: {
    int rlen = gap_row_len(&E.buf, E.cy);
    if (E.cx < rlen - 1) E.cx++;
  } break;

  case 'j':
  case KEY_DOWN:
    if (E.cy < total_lines - 1) {
      E.cy++;
      clamp_cx();
    }
    break;

  case 'k':
  case KEY_UP:
    if (E.cy > 0) {
      E.cy--;
      clamp_cx();
    }
    break;

  case '0':
    E.cx = 0;
    break;

  case '$': {
    int rlen = gap_row_len(&E.buf, E.cy);
    E.cx = rlen > 0 ? rlen - 1 : 0;
  } break;

  case 'w': {
    /* Next word: skip current word chars, then skip non-word chars */
    int pos = gap_pos_from_rc(&E.buf, E.cy, E.cx);
    int len = gap_length(&E.buf);
    /* Skip current word */
    while (pos < len && is_word_char(gap_char_at(&E.buf, pos)))
      pos++;
    /* Skip whitespace/punctuation */
    while (pos < len && !is_word_char(gap_char_at(&E.buf, pos)) &&
           gap_char_at(&E.buf, pos) != '\n')
      pos++;
    gap_rc_from_pos(&E.buf, pos, &E.cy, &E.cx);
    clamp_cx();
  } break;

  case 'b': {
    /* Previous word */
    int pos = gap_pos_from_rc(&E.buf, E.cy, E.cx);
    if (pos > 0) pos--;
    /* Skip whitespace/punctuation backwards */
    while (pos > 0 && !is_word_char(gap_char_at(&E.buf, pos)))
      pos--;
    /* Skip word backwards */
    while (pos > 0 && is_word_char(gap_char_at(&E.buf, pos - 1)))
      pos--;
    gap_rc_from_pos(&E.buf, pos, &E.cy, &E.cx);
  } break;

  case 'g':
    E.pending_g = 1;
    break;

  case 'G':
    E.cy = total_lines > 0 ? total_lines - 1 : 0;
    E.cx = 0;
    break;

  case KEY_HOME:
    E.cx = 0;
    break;

  case KEY_END: {
    int rlen = gap_row_len(&E.buf, E.cy);
    E.cx = rlen > 0 ? rlen - 1 : 0;
  } break;

  case KEY_PGUP: {
    int cr = E.rows - 3;
    E.cy -= cr;
    if (E.cy < 0) E.cy = 0;
    clamp_cx();
  } break;

  case KEY_PGDN: {
    int cr = E.rows - 3;
    E.cy += cr;
    if (E.cy >= total_lines) E.cy = total_lines - 1;
    if (E.cy < 0) E.cy = 0;
    clamp_cx();
  } break;

  /* ── Editing ── */
  case 'x':
    take_snapshot();
    gap_move(&E.buf, gap_pos_from_rc(&E.buf, E.cy, E.cx));
    gap_delete_fwd(&E.buf);
    E.dirty = 1;
    clamp_cx();
    break;

  case 'd':
    E.pending_d = 1;
    break;

  case 'u':
    if (E.undo_snap) {
      gap_restore(&E.buf, E.undo_snap, E.undo_len);
      E.cx = E.undo_cx;
      E.cy = E.undo_cy;
      E.dirty = 1;
      E.undo_snap = (void *)0;
      ui_set_status("Undone");
    } else {
      ui_set_status("Nothing to undo");
    }
    break;

  /* ── Search ── */
  case 'n':
    search_next();
    break;

  case 'N':
    search_prev();
    break;

  /* ── Mode switches ── */
  case 'i':
    take_snapshot();
    E.mode = MODE_INSERT;
    gap_move(&E.buf, gap_pos_from_rc(&E.buf, E.cy, E.cx));
    break;

  case 'a': {
    take_snapshot();
    E.mode = MODE_INSERT;
    int rlen = gap_row_len(&E.buf, E.cy);
    if (rlen > 0 && E.cx < rlen)
      E.cx++;
    gap_move(&E.buf, gap_pos_from_rc(&E.buf, E.cy, E.cx));
  } break;

  case 'o': {
    take_snapshot();
    E.mode = MODE_INSERT;
    int eol = gap_pos_from_rc(&E.buf, E.cy, gap_row_len(&E.buf, E.cy));
    gap_move(&E.buf, eol);
    gap_insert(&E.buf, '\n');
    E.cy++;
    E.cx = 0;
    E.dirty = 1;
  } break;

  case 'O': {
    take_snapshot();
    E.mode = MODE_INSERT;
    int sol = gap_pos_from_rc(&E.buf, E.cy, 0);
    gap_move(&E.buf, sol);
    gap_insert(&E.buf, '\n');
    gap_move(&E.buf, sol);
    E.cx = 0;
    E.dirty = 1;
  } break;

  case '/':
    E.mode = MODE_COMMAND;
    E.cmd[0] = '/';
    E.cmd_len = 1;
    break;

  case ':':
    E.mode = MODE_COMMAND;
    E.cmd[0] = '\0';
    E.cmd_len = 0;
    break;

  /* ── Menu ── */
  case KEY_ESC:
    E.mode = MODE_MENU;
    E.menu_cat = 0;
    E.menu_item = 0;
    break;

  case KEY_F1:
    E.mode = MODE_HELP;
    break;
  }

  /* Clear status on any key (except pending multi-key) */
  if (!E.pending_g && !E.pending_d)
    E.status[0] = '\0';
}

/* ── Insert mode ───────────────────────────────────────────────────────── */

static void handle_insert(int key) {
  switch (key) {
  case KEY_ESC:
    E.mode = MODE_NORMAL;
    /* Back up one if possible (vi convention) */
    if (E.cx > 0) E.cx--;
    clamp_cx();
    return;

  case KEY_BS:
  case KEY_BACKSPACE:
    if (E.buf.gap_start > 0) {
      int c = E.buf.data[E.buf.gap_start - 1];
      gap_delete(&E.buf);
      E.dirty = 1;
      if (c == '\n') {
        /* Moved up a line */
        E.cy--;
        E.cx = gap_row_len(&E.buf, E.cy);
      } else {
        E.cx--;
      }
    }
    return;

  case KEY_DELETE:
    gap_delete_fwd(&E.buf);
    E.dirty = 1;
    return;

  case KEY_ENTER:
    gap_insert(&E.buf, '\n');
    E.cy++;
    E.cx = 0;
    E.dirty = 1;
    return;

  case KEY_LEFT:
    if (E.cx > 0) E.cx--;
    gap_move(&E.buf, gap_pos_from_rc(&E.buf, E.cy, E.cx));
    return;

  case KEY_RIGHT: {
    int rlen = gap_row_len(&E.buf, E.cy);
    if (E.cx < rlen) E.cx++;
    gap_move(&E.buf, gap_pos_from_rc(&E.buf, E.cy, E.cx));
  } return;

  case KEY_UP:
    if (E.cy > 0) {
      E.cy--;
      gap_move(&E.buf, gap_pos_from_rc(&E.buf, E.cy, E.cx));
    }
    return;

  case KEY_DOWN:
    if (E.cy < gap_line_count(&E.buf) - 1) {
      E.cy++;
      gap_move(&E.buf, gap_pos_from_rc(&E.buf, E.cy, E.cx));
    }
    return;

  default:
    break;
  }

  /* Printable character — insert */
  if (key >= 32 && key < 127) {
    gap_insert(&E.buf, (char)key);
    E.cx++;
    E.dirty = 1;
  }
}

/* ── Menu mode ─────────────────────────────────────────────────────────── */

static void handle_menu(int key) {
  switch (key) {
  case KEY_ESC:
    E.mode = MODE_NORMAL;
    break;

  case 'h':
  case KEY_LEFT:
    E.menu_cat--;
    if (E.menu_cat < 0) E.menu_cat = MENU_CAT_COUNT - 1;
    E.menu_item = 0;
    break;

  case 'l':
  case KEY_RIGHT:
    E.menu_cat++;
    if (E.menu_cat >= MENU_CAT_COUNT) E.menu_cat = 0;
    E.menu_item = 0;
    break;

  case 'k':
  case KEY_UP:
    E.menu_item--;
    if (E.menu_item < 0)
      E.menu_item = pi_menus[E.menu_cat].count - 1;
    break;

  case 'j':
  case KEY_DOWN:
    E.menu_item++;
    if (E.menu_item >= pi_menus[E.menu_cat].count)
      E.menu_item = 0;
    break;

  case KEY_ENTER:
  case '\n':
    E.mode = MODE_NORMAL;
    if (menu_dispatch(E.menu_cat, E.menu_item))
      E.quit = 1;
    break;
  }
}

/* ── Command mode ──────────────────────────────────────────────────────── */

static void handle_command(int key) {
  switch (key) {
  case KEY_ESC:
    E.mode = MODE_NORMAL;
    E.cmd_len = 0;
    E.cmd[0] = '\0';
    break;

  case KEY_BS:
  case KEY_BACKSPACE:
    if (E.cmd_len > 0) {
      E.cmd_len--;
      E.cmd[E.cmd_len] = '\0';
    } else {
      E.mode = MODE_NORMAL;
    }
    break;

  case KEY_ENTER:
  case '\n':
    E.mode = MODE_NORMAL;
    /* Parse command */
    if (E.cmd_len == 0) {
      /* Empty — do nothing */
    } else if (E.cmd[0] == '/' && E.cmd_len > 1) {
      /* Search forward from cursor */
      uc_strncpy(E.search, E.cmd + 1, sizeof(E.search));
      E.search_len = E.cmd_len - 1;
      search_next();
    } else if (E.cmd[0] == 'q' && E.cmd_len == 1) {
      if (E.dirty)
        ui_set_status("Unsaved changes — use :q! to force");
      else
        E.quit = 1;
    } else if (E.cmd[0] == 'q' && E.cmd[1] == '!' && E.cmd_len == 2) {
      E.quit = 1;
    } else if (E.cmd[0] == 'w' && E.cmd[1] == 'q' && E.cmd_len == 2) {
      if (save_file(E.filename) == 0)
        E.quit = 1;
    } else if (E.cmd[0] == 'w' && E.cmd_len == 1) {
      save_file(E.filename);
    } else if (E.cmd[0] == 'w' && E.cmd[1] == ' ' && E.cmd_len > 2) {
      /* :w filename — save as */
      save_file(E.cmd + 2);
    } else if (E.cmd[0] == 'e' && E.cmd[1] == '!' && E.cmd_len == 2) {
      /* :e! — discard and reload (or new if no filename) */
      char saved[256];
      uc_strncpy(saved, E.filename, sizeof(saved));
      reset_buffer();
      if (saved[0])
        load_file(saved);
      else
        ui_set_status("(new file)");
    } else if (E.cmd[0] == 'e' && E.cmd[1] == ' ' && E.cmd_len > 2) {
      /* :e filename — open file */
      if (E.dirty) {
        ui_set_status("Unsaved changes — use :e! to discard");
      } else {
        reset_buffer();
        load_file(E.cmd + 2);
      }
    } else {
      /* Try as line number */
      int line = uc_atoi(E.cmd);
      if (line > 0) {
        E.cy = line - 1;
        int total = gap_line_count(&E.buf);
        if (E.cy >= total) E.cy = total - 1;
        E.cx = 0;
      } else {
        ui_set_status("Unknown command");
      }
    }
    E.cmd_len = 0;
    E.cmd[0] = '\0';
    break;

  default:
    /* Append to command buffer */
    if (key >= 32 && key < 127 && E.cmd_len < (int)sizeof(E.cmd) - 1) {
      E.cmd[E.cmd_len++] = (char)key;
      E.cmd[E.cmd_len] = '\0';
    }
    break;
  }
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
  /* Initialize terminal */
  term_raw();
  term_get_winsize(&E.rows, &E.cols);

  /* Initialize gap buffer */
  gap_init(&E.buf, 4096);

  /* Start in normal mode */
  E.mode = MODE_NORMAL;
  E.menu_cat = 0;
  E.menu_item = 0;

  /* Load file if specified */
  if (argc > 1)
    load_file(argv[1]);
  else
    ui_set_status("pi - PiPAPo Editor | Esc: menu");

  /* Main loop */
  while (!E.quit) {
    ui_refresh();
    int key = pi_read_key();
    if (key == KEY_NONE)
      continue;

    switch (E.mode) {
    case MODE_NORMAL:  handle_normal(key);  break;
    case MODE_INSERT:  handle_insert(key);  break;
    case MODE_MENU:    handle_menu(key);    break;
    case MODE_COMMAND: handle_command(key);  break;
    case MODE_HELP:    E.mode = MODE_NORMAL; break;
    }
  }

  /* Cleanup */
  term_clear();
  term_cursor_to(0, 0);
  term_attr_reset();
  term_cursor_show();
  term_restore();

  return 0;
}
