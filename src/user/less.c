/*
 * less.c — colorful pager (also installed as /bin/more via symlink).
 *
 * argv[0] decides the persona: invoked as "more" → forward-only, exits
 * at EOF; invoked as anything else → full less behavior.
 *
 * L-1 scope: terminal raw mode, winsize, persona detect, naive first-
 * screen dump, status bar, quit on 'q' / Ctrl-C (default SIGINT).
 * Forward paging, line index, search, line numbers, env vars and the
 * help overlay land in later phases.  See docs/proposals/less_pager.md.
 */

#include "lib/uclib.h"

#include <sys/ioctl.h>

#include "common/errno.h"
#include "common/poll.h"
#include "common/termios.h"

/* ── Color palette (matches top.c) ─────────────────────────────────────── */

#define C(s)      s
#define C_RST     C("\033[0m")
#define C_DIM     C("\033[2m")
#define C_BCYAN   C("\033[1;36m")
#define C_BYELLOW C("\033[1;33m")
#define C_REV     C("\033[7m")

/* ── Global state ──────────────────────────────────────────────────────── */

static int g_rows = 24;
static int g_cols = 80;
static int g_more_mode;          /* 1 when invoked as `more` */
static const char *g_filename;   /* for status line */
static int g_fd = -1;
static long g_filesize;

static struct termios g_saved_tios;
static int g_raw_active;

/* ── Persona ───────────────────────────────────────────────────────────── */

static const char *basename_of(const char *p) {
  const char *base = p;
  for (const char *s = p; *s; s++)
    if (*s == '/') base = s + 1;
  return base;
}

/* ── Terminal raw mode ─────────────────────────────────────────────────── */

static void term_raw(void) {
  struct termios t;
  if (ioctl(0, TCGETS, &t) != 0) return;

  const unsigned char *src = (const unsigned char *)&t;
  unsigned char *dst = (unsigned char *)&g_saved_tios;
  for (unsigned i = 0; i < sizeof(struct termios); i++) dst[i] = src[i];

  t.c_iflag &= ~(ICRNL | IXON);
  t.c_lflag &= ~(ICANON | ECHO);
  ioctl(0, TCSETS, &t);
  g_raw_active = 1;

  /* Disable DECAWM so writes to the last column don't latch a pending
   * scroll on VT emulators — we position chrome by absolute row/col. */
  write(1, "\033[?7l", 5);
}

static void term_restore(void) {
  if (!g_raw_active) return;
  write(1, "\033[?7h", 5);             /* re-enable autowrap */
  write(1, "\033[" "0m", 4);           /* reset SGR */
  ioctl(0, TCSETS, &g_saved_tios);
  g_raw_active = 0;
}

static void term_get_winsize(void) {
  struct winsize ws;
  if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
    g_rows = ws.ws_row;
    g_cols = ws.ws_col;
  } else {
    g_rows = 24;
    g_cols = 80;
  }
}

/* ── Key reader ────────────────────────────────────────────────────────── */

#define LKEY_NONE   0x100
#define LKEY_ESC    0x101
#define LKEY_UP     0x102
#define LKEY_DOWN   0x103
#define LKEY_LEFT   0x104
#define LKEY_RIGHT  0x105
#define LKEY_PGUP   0x106
#define LKEY_PGDN   0x107
#define LKEY_HOME   0x108
#define LKEY_END    0x109

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

static int read_key(void) {
  int c = read_byte();
  if (c < 0) return LKEY_NONE;
  if (c != 0x1b) return c;

  int c2 = read_byte_maybe();
  if (c2 < 0) return LKEY_ESC;

  if (c2 == '[') {
    int c3 = read_byte();
    if (c3 < 0) return LKEY_ESC;
    switch (c3) {
      case 'A': return LKEY_UP;
      case 'B': return LKEY_DOWN;
      case 'C': return LKEY_RIGHT;
      case 'D': return LKEY_LEFT;
      case 'H': return LKEY_HOME;
      case 'F': return LKEY_END;
    }
    if (c3 >= '0' && c3 <= '9') {
      int c4 = read_byte();
      if (c4 == '~') {
        switch (c3) {
          case '5': return LKEY_PGUP;
          case '6': return LKEY_PGDN;
          case '1': case '7': return LKEY_HOME;
          case '4': case '8': return LKEY_END;
        }
      }
      /* Swallow two-digit ~ sequences so they don't leak. */
      if (c4 >= '0' && c4 <= '9') read_byte();
    }
    return LKEY_NONE;
  }
  if (c2 == 'O') {
    int c3 = read_byte();
    switch (c3) {
      case 'H': return LKEY_HOME;
      case 'F': return LKEY_END;
    }
    return LKEY_NONE;
  }
  return LKEY_NONE;
}

/* ── Naive first-screen dump (placeholder until L-2) ───────────────────── */

static void draw_first_screen(void) {
  fputs("\033[2J\033[H", stdout);    /* clear, home */

  /* Read up to g_rows-1 lines from the top of the file, truncating at
   * g_cols.  This is just so L-1 shows something on screen — L-2
   * replaces it with the proper line-index pager. */
  lseek(g_fd, 0, 0);
  char buf[256];
  ssize_t n;
  int row = 0;
  int col = 0;
  int limit_rows = g_rows - 1;       /* leave one row for the status bar */

  while (row < limit_rows && (n = read(g_fd, buf, sizeof(buf))) > 0) {
    for (ssize_t i = 0; i < n && row < limit_rows; i++) {
      unsigned char c = (unsigned char)buf[i];
      if (c == '\n') {
        putchar('\n');
        row++;
        col = 0;
      } else if (c == '\t') {
        int tw = 8 - (col & 7);
        for (int k = 0; k < tw && col < g_cols; k++) {
          putchar(' ');
          col++;
        }
      } else if (c < 0x20 || c == 0x7f) {
        if (col < g_cols) { putchar('?'); col++; }
      } else {
        if (col < g_cols) { putchar((char)c); col++; }
      }
    }
  }
  /* Pad remaining rows so the status bar always sits at the bottom. */
  while (row < limit_rows) {
    putchar('\n');
    row++;
  }
}

static void draw_status(void) {
  /* Status bar on the last row, reverse-video.  L-1 just shows the
   * filename and persona + geometry; position/percent come with L-2. */
  printf("\033[%d;1H", g_rows);
  fputs(C_REV, stdout);
  if (g_more_mode) {
    fputs(C_BYELLOW, stdout);
    fputs("--More--", stdout);
    fputs(C_RST C_REV, stdout);
    fputs(" ", stdout);
  }
  fputs(C_BCYAN, stdout);
  fputs(g_filename ? g_filename : "(stdin)", stdout);
  fputs(C_RST C_REV, stdout);
  printf("  %u bytes  %dx%d  ", (unsigned)g_filesize, g_cols, g_rows);
  fputs(C_DIM "(q to quit)" C_RST, stdout);
  fputs("\033[K", stdout);            /* clear to EOL inside reverse */
  fputs(C_RST, stdout);
  fflush(stdout);
}

/* ── Main loop ─────────────────────────────────────────────────────────── */

static int run(void) {
  term_get_winsize();
  term_raw();
  draw_first_screen();
  draw_status();

  for (;;) {
    int k = read_key();
    if (k == 'q' || k == 0x03 /* ^C */) break;
    /* Any other key: ignore in L-1; L-2 will wire paging here.
     * Re-query winsize each iteration so SIGWINCH-less terminals
     * still pick up resizes between key presses. */
    int prev_rows = g_rows, prev_cols = g_cols;
    term_get_winsize();
    if (g_rows != prev_rows || g_cols != prev_cols) {
      draw_first_screen();
      draw_status();
    }
  }
  return 0;
}

/* ── Entry ─────────────────────────────────────────────────────────────── */

static void usage(int more_mode) {
  if (more_mode) {
    fputs("Usage: more FILE\n", stdout);
  } else {
    fputs("Usage: less FILE\n"
          "  Interactive pager.  Press q to quit.\n", stdout);
  }
}

int main(int argc, char *argv[]) {
  g_more_mode = (strcmp(basename_of(argv[0]), "more") == 0);

  if (argc < 2 || strcmp(argv[1], "--help") == 0) {
    usage(g_more_mode);
    return (argc < 2) ? 1 : 0;
  }
  if (strcmp(argv[1], "-") == 0) {
    fputs(g_more_mode ? "more: " : "less: ", stderr);
    fputs("stdin not seekable (pipe pager not supported yet)\n", stderr);
    return 1;
  }

  g_filename = argv[1];
  g_fd = open(g_filename, O_RDONLY, 0);
  if (g_fd < 0) {
    fputs(g_more_mode ? "more: " : "less: ", stderr);
    fputs(g_filename, stderr);
    fputs(": cannot open\n", stderr);
    return 1;
  }

  /* File size via lseek(SEEK_END) — keeps the loader-format dependency
   * out of the inner loop and matches what `wc -c` would report. */
  g_filesize = lseek(g_fd, 0, 2 /* SEEK_END */);
  if (g_filesize < 0) g_filesize = 0;

  int rc = run();

  term_restore();
  /* Cursor to a clean line below the pager before returning to the shell. */
  fputs("\033[" "0m", stdout);
  printf("\033[%d;1H\n", g_rows);
  fflush(stdout);

  close(g_fd);
  return rc;
}
