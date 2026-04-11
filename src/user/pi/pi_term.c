/*
 * pi_term.c — Terminal control for PiPAPo Editor
 *
 * Raw mode, key reader (handles escape sequences for arrows, F-keys),
 * ANSI output helpers, and terminal size query.
 */

#include "pi.h"

/* ── Raw mode ──────────────────────────────────────────────────────────── */

static struct termios orig_termios;
static int raw_active;

void term_raw(void) {
  struct termios t;
  ioctl(0, TCGETS, &t);

  /* Save original for restore */
  const unsigned char *src = (const unsigned char *)&t;
  unsigned char *dst = (unsigned char *)&orig_termios;
  for (unsigned i = 0; i < sizeof(struct termios); i++)
    dst[i] = src[i];

  /* Raw: no ICANON, no ECHO, no ICRNL, no IXON.
   * Keep ISIG so Ctrl-C still works.
   * Keep OPOST|ONLCR so \n outputs as \r\n. */
  t.c_iflag &= ~(ICRNL | IXON);
  t.c_lflag &= ~(ICANON | ECHO);

  ioctl(0, TCSETS, &t);
  raw_active = 1;
}

void term_restore(void) {
  if (raw_active) {
    ioctl(0, TCSETS, &orig_termios);
    raw_active = 0;
  }
}

/* ── Terminal size ─────────────────────────────────────────────────────── */

void term_get_winsize(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
    *rows = ws.ws_row;
    *cols = ws.ws_col;
  } else {
    *rows = 24;
    *cols = 80;
  }
}

/* ── Key reader ────────────────────────────────────────────────────────── */

static int read_byte(void) {
  unsigned char c;
  ssize_t n = read(0, &c, 1);
  if (n <= 0)
    return -1;
  return c;
}

/* Try to read one more byte with a very short implicit timeout.
 * If nothing is available, return -1 (standalone Esc). */
static int read_byte_maybe(void) {
  struct pollfd pfd;
  pfd.fd = 0;
  pfd.events = 1; /* POLLIN */
  pfd.revents = 0;
  /* 50ms timeout — enough for escape sequences, short enough to feel instant */
  struct { long tv_sec; long tv_nsec; } ts = {0, 50000000};
  int r = ppoll(&pfd, 1, &ts, (void *)0, 0);
  if (r > 0)
    return read_byte();
  return -1;
}

int pi_read_key(void) {
  int c = read_byte();
  if (c < 0)
    return KEY_NONE;

  /* Not an escape — return raw byte */
  if (c != 0x1b)
    return c;

  /* Esc pressed — check if this is a sequence or standalone Esc */
  int c2 = read_byte_maybe();
  if (c2 < 0)
    return KEY_ESC; /* standalone Esc */

  if (c2 == '[') {
    /* CSI sequence: \033[ ... */
    int c3 = read_byte();
    if (c3 < 0)
      return KEY_ESC;

    /* Arrow keys and simple sequences */
    switch (c3) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    }

    /* Extended sequences: \033[N~ */
    if (c3 >= '0' && c3 <= '9') {
      int c4 = read_byte();
      if (c4 == '~') {
        switch (c3) {
        case '1': return KEY_HOME;
        case '3': return KEY_DELETE;
        case '4': return KEY_END;
        case '5': return KEY_PGUP;
        case '6': return KEY_PGDN;
        case '7': return KEY_HOME;
        case '8': return KEY_END;
        }
      }
      /* Two-digit codes: \033[NN~ */
      if (c4 >= '0' && c4 <= '9') {
        int c5 = read_byte();
        if (c5 == '~') {
          int code = (c3 - '0') * 10 + (c4 - '0');
          switch (code) {
          case 11: return KEY_F1;
          case 21: return KEY_F10;
          }
        }
      }
    }
    /* Unknown CSI — discard */
    return KEY_NONE;
  }

  if (c2 == 'O') {
    /* SS3 sequence: \033O... (xterm F1-F4, Home, End) */
    int c3 = read_byte();
    switch (c3) {
    case 'P': return KEY_F1;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    }
    return KEY_NONE;
  }

  /* Unknown escape — discard the second byte */
  return KEY_NONE;
}

/* ── ANSI output helpers ───────────────────────────────────────────────── */

static char ansi_buf[32];

static void emit(const char *s) {
  int len = 0;
  while (s[len]) len++;
  write(1, s, len);
}

static void emit_csi_n(const char *prefix, int n, char suffix) {
  /* Build \033[<n><suffix> */
  char *p = ansi_buf;
  *p++ = '\033';
  *p++ = '[';
  if (prefix) {
    while (*prefix)
      *p++ = *prefix++;
  }
  if (n >= 100) *p++ = '0' + n / 100;
  if (n >= 10)  *p++ = '0' + (n / 10) % 10;
  *p++ = '0' + n % 10;
  *p++ = suffix;
  write(1, ansi_buf, p - ansi_buf);
}

void term_clear(void) {
  emit("\033[2J\033[H");
}

void term_cursor_to(int row, int col) {
  /* \033[row;colH  (1-based) */
  char *p = ansi_buf;
  *p++ = '\033';
  *p++ = '[';
  int r = row + 1;
  int c = col + 1;
  if (r >= 100) *p++ = '0' + r / 100;
  if (r >= 10)  *p++ = '0' + (r / 10) % 10;
  *p++ = '0' + r % 10;
  *p++ = ';';
  if (c >= 100) *p++ = '0' + c / 100;
  if (c >= 10)  *p++ = '0' + (c / 10) % 10;
  *p++ = '0' + c % 10;
  *p++ = 'H';
  write(1, ansi_buf, p - ansi_buf);
}

void term_cursor_hide(void) { emit("\033[?25l"); }
void term_cursor_show(void) { emit("\033[?25h"); }

void term_attr_reset(void)   { emit("\033[0m"); }
void term_attr_reverse(void) { emit("\033[7m"); }
void term_attr_dim(void)     { emit("\033[2m"); }
void term_attr_bold(void)    { emit("\033[1m"); }

void term_attr_fg(int color) { emit_csi_n("", 30 + color, 'm'); }
void term_attr_bg(int color) { emit_csi_n("", 40 + color, 'm'); }
