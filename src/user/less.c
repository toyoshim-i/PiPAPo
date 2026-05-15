/*
 * less.c — colorful pager (also installed as /bin/more via symlink).
 *
 * argv[0] decides the persona: invoked as "more" → forward-only, exits
 * at EOF; invoked as anything else → full less behavior.
 *
 * Current scope (L-2): persona detect, raw mode, lazy forward paging by
 * streamed reads (no full line index yet), reverse-video status bar with
 * filename / line / percent / EOF marker, `more` auto-exits when the
 * viewport reaches EOF.
 *
 * Backward paging, search, line numbers, env vars and the help overlay
 * land in later phases.  See docs/proposals/less_pager.md.
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
#define C_BGREEN  C("\033[1;32m")
#define C_REV     C("\033[7m")

/* ── Global state ──────────────────────────────────────────────────────── */

static int g_rows = 24;
static int g_cols = 80;
static int g_more_mode;          /* 1 when invoked as `more` */
static const char *g_filename;
static int g_fd = -1;
static long g_filesize;

/* Viewport position.  g_top_off is the byte offset of the first character
 * shown on screen; g_top_line is its 1-based line number.  After every
 * render we also know g_next_off: the byte offset *just past* the last
 * character drawn — i.e. where the next screen would start.  When the
 * renderer hits EOF before filling the viewport, g_eof is set. */
static long g_top_off;
static long g_top_line = 1;
static long g_next_off;
static long g_next_line;
static int  g_eof;

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
  write(1, "\033[?7h", 5);
  write(1, "\033[" "0m", 4);
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

/* ── Renderer ─────────────────────────────────────────────────────────── */

/* Render up to (g_rows - 1) logical lines starting at byte offset
 * `start_off`.  Long lines are truncated at g_cols (wrap toggle is a
 * later phase).  Updates g_next_off / g_next_line / g_eof as side
 * effects so the main loop can advance the viewport for the next page. */
static void draw_view(long start_off, long start_line) {
  fputs("\033[2J\033[H", stdout);

  lseek(g_fd, start_off, 0);

  char buf[256];
  ssize_t n;
  int row = 0;
  int col = 0;
  int limit = g_rows - 1;
  long pos = start_off;
  long line = start_line;
  int saw_any_in_line = 0;
  int hit_eof = 0;

  while (row < limit) {
    n = read(g_fd, buf, sizeof(buf));
    if (n <= 0) { hit_eof = 1; break; }
    for (ssize_t i = 0; i < n && row < limit; i++) {
      unsigned char c = (unsigned char)buf[i];
      pos++;
      if (c == '\n') {
        putchar('\n');
        row++;
        col = 0;
        line++;
        saw_any_in_line = 0;
      } else if (c == '\t') {
        int tw = 8 - (col & 7);
        for (int k = 0; k < tw && col < g_cols; k++) {
          putchar(' ');
          col++;
        }
        saw_any_in_line = 1;
      } else if (c < 0x20 || c == 0x7f) {
        if (col < g_cols) { putchar('?'); col++; }
        saw_any_in_line = 1;
      } else {
        if (col < g_cols) { putchar((char)c); col++; }
        saw_any_in_line = 1;
      }
    }
  }

  /* If we hit EOF mid-line (no trailing newline), the partial line still
   * occupied a visual row — count it so the viewport math stays honest. */
  if (hit_eof && saw_any_in_line) {
    row++;
    line++;
  }

  /* Pad the viewport so the status bar always sits on the last row. */
  while (row < limit) {
    putchar('\n');
    row++;
  }

  g_next_off = pos;
  g_next_line = line;
  g_eof = hit_eof;
}

/* ── Status bar ────────────────────────────────────────────────────────── */

static void draw_status(void) {
  unsigned pct = 0;
  if (g_filesize > 0) {
    long base = g_eof ? g_filesize : g_next_off;
    pct = (unsigned)((base * 100) / g_filesize);
    if (pct > 100) pct = 100;
  } else {
    pct = 100;
  }

  printf("\033[%d;1H", g_rows);
  fputs(C_REV, stdout);
  if (g_more_mode) {
    fputs(C_BYELLOW, stdout);
    fputs("--More--", stdout);
    fputs(C_RST C_REV " ", stdout);
  }
  fputs(C_BCYAN, stdout);
  fputs(g_filename ? g_filename : "(stdin)", stdout);
  fputs(C_RST C_REV, stdout);
  fputs("  ", stdout);
  fputs(C_BYELLOW, stdout);
  printf("line %u", (unsigned)g_top_line);
  fputs(C_RST C_REV " · ", stdout);
  fputs(C_BYELLOW, stdout);
  printf("%u%%", pct);
  fputs(C_RST C_REV, stdout);
  if (g_eof) {
    fputs("  ", stdout);
    fputs(C_BGREEN, stdout);
    fputs(g_more_mode ? "(END — any key)" : "(END)", stdout);
    fputs(C_RST C_REV, stdout);
  }
  fputs("  ", stdout);
  fputs(C_DIM "(q to quit)" C_RST, stdout);
  fputs("\033[K", stdout);
  fputs(C_RST, stdout);
  fflush(stdout);
}

static void redraw(void) {
  draw_view(g_top_off, g_top_line);
  draw_status();
}

/* ── Navigation ───────────────────────────────────────────────────────── */

static void page_forward(void) {
  if (g_eof) return;
  g_top_off = g_next_off;
  g_top_line = g_next_line;
  redraw();
}

static void line_forward(void) {
  /* Advance to the next line: scan from g_top_off for the first '\n',
   * set g_top_off to the byte after it.  If no newline before EOF,
   * we're at the last line — do nothing. */
  if (g_eof && g_next_off >= g_filesize) return;
  lseek(g_fd, g_top_off, 0);
  char buf[256];
  ssize_t n;
  long pos = g_top_off;
  int found = 0;
  while ((n = read(g_fd, buf, sizeof(buf))) > 0) {
    for (ssize_t i = 0; i < n; i++) {
      pos++;
      if (buf[i] == '\n') { found = 1; break; }
    }
    if (found) break;
  }
  if (!found) return;
  g_top_off = pos;
  g_top_line++;
  redraw();
}

/* ── Main loop ─────────────────────────────────────────────────────────── */

static int run(void) {
  term_get_winsize();
  term_raw();
  redraw();

  for (;;) {
    int prev_rows = g_rows, prev_cols = g_cols;

    int k = read_key();

    /* `q` and Ctrl-C always quit. */
    if (k == 'q' || k == 0x03) break;

    /* `more` mode: at EOF, any keypress exits (matches real `more`). */
    if (g_more_mode && g_eof) break;

    /* Forward paging.  Both modes accept Space / f / PgDn / Down. */
    if (k == ' ' || k == 'f' || k == LKEY_PGDN) {
      page_forward();
      if (g_more_mode && g_eof) {
        /* Reached EOF while paging: redraw to surface "(END)" then wait
         * one more keystroke before quitting, so the user actually sees
         * the last screen. */
        continue;
      }
    } else if (k == '\n' || k == '\r' || k == 'j' || k == LKEY_DOWN) {
      line_forward();
    }

    term_get_winsize();
    if (g_rows != prev_rows || g_cols != prev_cols) redraw();
  }
  return 0;
}

/* ── Entry ─────────────────────────────────────────────────────────────── */

static void usage(int more_mode) {
  if (more_mode) {
    fputs("Usage: more FILE\n"
          "  Page forward through FILE.  Space=next screen, Enter=one line,\n"
          "  q=quit (auto-exits at EOF).\n", stdout);
  } else {
    fputs("Usage: less FILE\n"
          "  Interactive pager.  Space/f=page forward, Enter/j=line forward,\n"
          "  k=line back, q=quit.\n", stdout);
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

  g_filesize = lseek(g_fd, 0, 2 /* SEEK_END */);
  if (g_filesize < 0) g_filesize = 0;

  int rc = run();

  term_restore();
  fputs("\033[" "0m", stdout);
  printf("\033[%d;1H\n", g_rows);
  fflush(stdout);

  close(g_fd);
  return rc;
}
