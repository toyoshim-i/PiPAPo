/*
 * pile.c — PiPAPo two-pane filer, entry point and main loop
 *
 * Design: docs/proposals/pile.md
 *
 * P1 scope: single-pane listing, cursor navigation, ENTER on directory,
 * quit with F10 / q / Ctrl-Q.  No file operations, no viewer.
 */

#include "pile.h"

#include "common/errno.h"
#include "common/poll.h"

/* ── Global state ─────────────────────────────────────────────────────── */

pile_pane_t pile_pane_a;
pile_pane_t pile_pane_b;
pile_pane_t *pile_active = &pile_pane_a;

int pile_rows;
int pile_cols;
int pile_layout;
int pile_quit;
int pile_use_color = 1;

/* ── Terminal raw mode ────────────────────────────────────────────────── */

static struct termios saved_tios;
static int raw_active;

void pile_term_raw(void) {
  struct termios t;
  if (ioctl(0, TCGETS, &t) != 0) return;

  const unsigned char *src = (const unsigned char *)&t;
  unsigned char *dst = (unsigned char *)&saved_tios;
  for (unsigned i = 0; i < sizeof(struct termios); i++) dst[i] = src[i];

  t.c_iflag &= ~(ICRNL | IXON);
  t.c_lflag &= ~(ICANON | ECHO);
  ioctl(0, TCSETS, &t);
  raw_active = 1;
}

void pile_term_restore(void) {
  if (raw_active) {
    ioctl(0, TCSETS, &saved_tios);
    raw_active = 0;
  }
}

/* ── Key reader ───────────────────────────────────────────────────────── */

static int read_byte(void) {
  unsigned char c;
  for (;;) {
    ssize_t n = read(0, &c, 1);
    if (n == 1) return c;
    if (n < 0 && -n == EINTR) continue;
    return -1;
  }
}

static int read_byte_maybe(void) {
  struct pollfd pfd;
  pfd.fd = 0;
  pfd.events = 1;
  pfd.revents = 0;
  struct { long tv_sec; long tv_nsec; } ts = {0, 50000000};
  int r = ppoll(&pfd, 1, &ts, (void *)0, 0);
  if (r > 0) return read_byte();
  return -1;
}

int pile_read_key(void) {
  int c = read_byte();
  if (c < 0) return PKEY_NONE;
  if (c != 0x1b) return c;

  int c2 = read_byte_maybe();
  if (c2 < 0) return PKEY_ESC;

  if (c2 == '[') {
    int c3 = read_byte();
    if (c3 < 0) return PKEY_ESC;
    switch (c3) {
      case 'A': return PKEY_UP;
      case 'B': return PKEY_DOWN;
      case 'C': return PKEY_RIGHT;
      case 'D': return PKEY_LEFT;
      case 'H': return PKEY_HOME;
      case 'F': return PKEY_END;
    }
    if (c3 >= '0' && c3 <= '9') {
      int c4 = read_byte();
      if (c4 == '~') {
        switch (c3) {
          case '1': return PKEY_HOME;
          case '3': return PKEY_DELETE;
          case '4': return PKEY_END;
          case '5': return PKEY_PGUP;
          case '6': return PKEY_PGDN;
          case '7': return PKEY_HOME;
          case '8': return PKEY_END;
        }
      }
      if (c4 >= '0' && c4 <= '9') {
        int c5 = read_byte();
        if (c5 == '~') {
          int code = (c3 - '0') * 10 + (c4 - '0');
          switch (code) {
            case 11: return PKEY_F1;
            case 13: return PKEY_F3;
            case 14: return PKEY_F4;
            case 15: return PKEY_F5;
            case 17: return PKEY_F6;
            case 18: return PKEY_F7;
            case 19: return PKEY_F8;
            case 20: return PKEY_F9;
            case 21: return PKEY_F10;
          }
        }
      }
    }
    return PKEY_NONE;
  }

  if (c2 == 'O') {
    int c3 = read_byte();
    switch (c3) {
      case 'P': return PKEY_F1;
      case 'R': return PKEY_F3;
      case 'S': return PKEY_F4;
      case 'H': return PKEY_HOME;
      case 'F': return PKEY_END;
    }
    return PKEY_NONE;
  }

  return PKEY_NONE;
}

/* ── Winsize and layout ───────────────────────────────────────────────── */

static void query_winsize(void) {
  struct winsize ws;
  if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
    pile_rows = ws.ws_row;
    pile_cols = ws.ws_col;
  } else {
    pile_rows = 25;
    pile_cols = 80;
  }
}

/* ── Key dispatch ─────────────────────────────────────────────────────── */

static void handle_key(int key) {
  pile_pane_t *p = pile_active;
  int vrows = pile_draw_visible_rows();

  switch (key) {
    case PKEY_F10:
    case 'q':
    case 0x11:  /* Ctrl-Q */
      pile_quit = 1;
      return;

    case PKEY_TAB:
      pile_active =
          (pile_active == &pile_pane_a) ? &pile_pane_b : &pile_pane_a;
      return;

    case PKEY_UP:
    case 'k':
      pile_pane_move(p, -1, vrows);
      return;
    case PKEY_DOWN:
    case 'j':
      pile_pane_move(p, +1, vrows);
      return;
    case PKEY_PGUP:
      pile_pane_move(p, -vrows, vrows);
      return;
    case PKEY_PGDN:
      pile_pane_move(p, +vrows, vrows);
      return;
    case PKEY_HOME:
    case 'g':
      pile_pane_home(p);
      return;
    case PKEY_END:
    case 'G':
      pile_pane_end(p, vrows);
      return;

    case PKEY_ENTER:
      pile_pane_enter(p);
      return;
    case PKEY_BS:
    case PKEY_BACKSPACE:
      pile_pane_parent(p);
      return;
  }
}

/* ── Entry ────────────────────────────────────────────────────────────── */

static void init_pane(pile_pane_t *p, const char *start) {
  int n = uc_strlen(start);
  if (n >= PILE_PATH_MAX) n = PILE_PATH_MAX - 1;
  uc_memcpy(p->path, start, n);
  p->path[n] = '\0';
  pile_pane_load(p);
}

int main(int argc, char *argv[]) {
  const char *start = ".";
  for (int i = 1; i < argc; i++) {
    if (uc_strcmp(argv[i], "--no-color") == 0) {
      pile_use_color = 0;
    } else if (uc_strcmp(argv[i], "--help") == 0) {
      uc_puts(
          "Usage: pile [--no-color] [path]\n"
          "  Two-pane filer.  F10 / q quits.\n");
      return 0;
    } else if (argv[i][0] != '-') {
      start = argv[i];
    } else {
      uc_eputs("pile: unknown option: ");
      uc_eputs(argv[i]);
      uc_eputs("\n");
      return 1;
    }
  }

  query_winsize();
  if (pile_cols < PILE_MIN_COLS) {
    uc_eputs("pile: terminal too narrow (need >=40 cols)\n");
    return 1;
  }
  pile_layout =
      (pile_cols >= PILE_TWOPANE_COLS) ? PILE_LAYOUT_TWO : PILE_LAYOUT_SINGLE;

  pile_term_raw();
  init_pane(&pile_pane_a, start);
  init_pane(&pile_pane_b, start);

  while (!pile_quit) {
    pile_draw_all();
    int k = pile_read_key();
    if (k == PKEY_NONE) continue;
    handle_key(k);
  }

  pile_draw_clear();
  pile_term_restore();
  return 0;
}
