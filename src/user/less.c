/*
 * less.c — colorful pager (also installed as /bin/more via symlink).
 *
 * argv[0] decides the persona: invoked as "more" → forward-only, exits
 * at EOF; invoked as anything else → full less behavior.
 *
 * Current scope (L-5, polish complete): persona detect, raw mode,
 * forward + backward paging via a lazy line-offset index, literal
 * `/pat` and `?pat` search with n/N repeat + reverse-video hit
 * highlighting, optional dim left-side line-number gutter (-N), wrap
 * vs chop (-S), TABS / LESS environment variables, full-screen help
 * overlay (h).  `more` stays forward-only and auto-exits at EOF.
 *
 * See docs/proposals/less_pager.md.
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

/* Lazy line-offset index.  g_lines[i] is the byte offset where line
 * (i + 1) starts; g_lines[0] is always 0.  Backed by a fixed-size BSS
 * array — most PPAP apps that need a heap seed `uc_heap_init` with a
 * static pool, and an index of this shape is just as well served by a
 * plain array.  Past IDX_MAX, navigation that needs an unindexed line
 * clamps to the highest indexed line — `G` on a 50 K-line log lands
 * at the cap. */
#define IDX_MAX 4096

static uint32_t g_lines[IDX_MAX];
static unsigned g_lines_count;   /* number of valid entries */
static long     g_scan_off;      /* file bytes examined for newlines */
static int      g_index_full;    /* 1 once g_scan_off >= g_filesize */

/* Search state.  g_pattern is a NUL-terminated literal string (no
 * regex); g_pat_len is its length for fast inner-loop comparison.
 * g_search_dir is +1 for forward (last /pat), -1 for backward (?pat),
 * used by `n` / `N`.  g_msg holds a transient status message such as
 * "Pattern not found" that overlays the right side of the status bar
 * until the next non-search keystroke. */
#define MAX_PAT 64
#define MAX_HL  32          /* per-viewport highlight cap */
#define MAX_MSG 48

static char  g_pattern[MAX_PAT + 1];
static int   g_pat_len;
static int   g_search_dir;    /* +1 forward, -1 backward, 0 = no pattern */
static long  g_last_match_off;/* byte offset of last successful hit, -1 if none */
static char  g_msg[MAX_MSG + 1];

/* Buffer for the current viewport's raw bytes — sized large enough for
 * any realistic 24×80 text screen (1920 visible cells; we cap at 4 KB
 * to absorb long-line tails that get truncated visually).  Loading the
 * page in one shot lets us do match-highlighting in a single render
 * pass without keeping a sliding window over the fd. */
#define VIEW_BUFSZ 4096
static char g_view_buf[VIEW_BUFSZ];

/* Display options.  All three can be toggled live; their initial
 * state is set by the LESS environment variable (-N, -S, -M) before
 * argv flags get a chance to override. */
static int g_show_lineno;    /* -N: prefix each line with its 1-based number */
static int g_chop_long;      /* -S: truncate long lines instead of wrapping */
static int g_long_status;    /* -M: include total line count in the status */
static int g_tab_width = 8;  /* TABS env var, 1..16 */

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

/* ── Lazy line-offset index ───────────────────────────────────────────── */

static void idx_init(void) {
  g_lines[0] = 0;
  g_lines_count = 1;
  g_scan_off = 0;
  g_index_full = 0;
}

static int idx_append(uint32_t off) {
  if (g_lines_count >= IDX_MAX) return 0;
  g_lines[g_lines_count++] = off;
  return 1;
}

/* Ensure g_lines_count > target_idx, reading the file forward from
 * g_scan_off and appending an entry after each '\n'.  Stops on EOF, at
 * the cap, or once target_idx is indexed.  Pass UINT_MAX-ish to index
 * the whole file (used by `G`). */
static void idx_extend(unsigned target_idx) {
  if (g_index_full) return;
  if (g_lines_count > target_idx) return;

  lseek(g_fd, g_scan_off, 0);
  char buf[256];
  while (g_lines_count <= target_idx) {
    ssize_t n = read(g_fd, buf, sizeof(buf));
    if (n <= 0) { g_index_full = 1; break; }
    for (ssize_t i = 0; i < n; i++) {
      g_scan_off++;
      if (buf[i] == '\n') {
        /* A newline at the very last byte of the file means no new line
         * begins after it — don't index a phantom line. */
        if (g_scan_off < g_filesize) {
          if (!idx_append((uint32_t)g_scan_off)) {
            /* Hit IDX_MAX; navigation past this point clamps to the
             * highest indexed line.  Stop scanning to save time. */
            return;
          }
          if (g_lines_count > target_idx) return;
        }
      }
    }
    if (g_scan_off >= g_filesize) { g_index_full = 1; break; }
  }
}

/* ── Renderer ─────────────────────────────────────────────────────────── */

/* Compute the width of the optional left-side line-number gutter,
 * including its trailing space.  Width tracks the digit count of the
 * highest indexed line so the gutter doesn't churn as `G` discovers a
 * wider count.  Returns 0 when -N is off. */
static int gutter_width(void) {
  if (!g_show_lineno) return 0;
  unsigned ln = g_lines_count ? g_lines_count : 1;
  int digits = 1;
  while (ln >= 10) { ln /= 10; digits++; }
  return digits + 1; /* digits + one space */
}

/* Render up to (g_rows - 1) logical lines starting at byte offset
 * `start_off`.  Reads the viewport's bytes into g_view_buf in one shot
 * so the matcher can highlight `g_pattern` hits inline.  In chop mode
 * (-S) long lines are truncated at the right edge; otherwise they wrap
 * onto the next visual row.  Tabs expand to the next multiple of
 * g_tab_width (TABS env var, default 8).
 *
 * If g_view_buf isn't large enough to cover (g_rows - 1) lines, the
 * renderer stops at the buffer end — g_next_off advances by what was
 * shown, so the next page-down continues from there.  4 KB easily
 * covers any sane text-file viewport. */
/* Emit the dim line-number gutter at the current cursor position.
 * When `is_continuation` is true (a wrap continuation row) the number
 * is replaced with blanks so the eye doesn't read it as a new line. */
static void emit_gutter(long ln, int width, int is_continuation) {
  if (width <= 0) return;
  fputs(C_DIM, stdout);
  if (is_continuation) {
    for (int i = 0; i < width; i++) putchar(' ');
  } else {
    char buf[12];
    int n = 0;
    unsigned v = (unsigned)ln;
    if (v == 0) { buf[n++] = '0'; }
    else { while (v > 0 && n < (int)sizeof(buf)) { buf[n++] = (char)('0' + v % 10); v /= 10; } }
    int pad = width - 1 - n;
    for (int i = 0; i < pad; i++) putchar(' ');
    for (int i = n - 1; i >= 0; i--) putchar(buf[i]);
    putchar(' ');
  }
  fputs(C_RST, stdout);
}

/* Rendering state passed through the per-cell helper.  Kept compact
 * because the renderer's inner loop pivots on these fields. */
typedef struct render_state {
  int  row, col;
  int  hl_left;
  int  gw;
  int  content_cols;
  int  limit;
  long line;
  int  saw_any_in_line;
} render_state_t;

/* Print one visible cell, handling wrap-vs-chop and SGR continuity
 * across wraps.  Returns 1 when the row budget is exhausted and the
 * caller must stop rendering, 0 otherwise. */
static int draw_cell(render_state_t *s, char ch) {
  if (s->col >= s->content_cols) {
    if (g_chop_long) return 0;       /* truncate: silently drop the byte */
    if (s->hl_left > 0) fputs(C_RST, stdout);
    putchar('\n');
    s->row++;
    if (s->row >= s->limit) return 1;
    emit_gutter(s->line, s->gw, 1);
    s->col = 0;
    if (s->hl_left > 0) fputs(C_REV, stdout);
  }
  putchar(ch);
  s->col++;
  s->saw_any_in_line = 1;
  return 0;
}

static void draw_view(long start_off, long start_line) {
  fputs("\033[2J\033[H", stdout);

  lseek(g_fd, start_off, 0);
  ssize_t vb_len = read(g_fd, g_view_buf, VIEW_BUFSZ);
  if (vb_len < 0) vb_len = 0;
  int read_to_eof = (start_off + vb_len >= g_filesize);

  int hl_starts[MAX_HL];
  int hl_count = 0;
  if (g_pat_len > 0 && vb_len >= g_pat_len) {
    for (ssize_t i = 0; i + g_pat_len <= vb_len && hl_count < MAX_HL; i++) {
      int match = 1;
      for (int j = 0; j < g_pat_len; j++) {
        if (g_view_buf[i + j] != g_pattern[j]) { match = 0; break; }
      }
      if (match) hl_starts[hl_count++] = (int)i;
    }
  }

  render_state_t s = {0};
  s.gw = gutter_width();
  s.content_cols = g_cols - s.gw;
  if (s.content_cols < 1) s.content_cols = 1;
  s.limit = g_rows - 1;
  s.line = start_line;

  emit_gutter(s.line, s.gw, 0);

  int hl_idx = 0;
  ssize_t i;
  int budget_exhausted = 0;

  for (i = 0; i < vb_len && s.row < s.limit && !budget_exhausted; i++) {
    if (s.hl_left == 0 && hl_idx < hl_count && (int)i == hl_starts[hl_idx]) {
      fputs(C_REV, stdout);
      s.hl_left = g_pat_len;
    }
    unsigned char c = (unsigned char)g_view_buf[i];
    if (c == '\n') {
      if (s.hl_left > 0) {
        fputs(C_RST, stdout);
        s.hl_left = 0;
        hl_idx++;
      }
      putchar('\n');
      s.row++;
      s.col = 0;
      s.line++;
      s.saw_any_in_line = 0;
      if (s.row < s.limit) emit_gutter(s.line, s.gw, 0);
      continue;
    }
    if (c == '\t') {
      int target = ((s.col / g_tab_width) + 1) * g_tab_width;
      while (s.col < target) {
        if (draw_cell(&s, ' ')) { budget_exhausted = 1; break; }
        if (g_chop_long && s.col >= s.content_cols) break;
      }
    } else if (c < 0x20 || c == 0x7f) {
      if (draw_cell(&s, '?')) budget_exhausted = 1;
    } else {
      if (draw_cell(&s, (char)c)) budget_exhausted = 1;
    }
    if (s.hl_left > 0) {
      s.hl_left--;
      if (s.hl_left == 0) {
        fputs(C_RST, stdout);
        hl_idx++;
      }
    }
  }
  if (s.hl_left > 0) fputs(C_RST, stdout);

  int hit_eof = (i >= vb_len) && read_to_eof;
  if (hit_eof && s.saw_any_in_line) {
    s.row++;
    s.line++;
  }
  while (s.row < s.limit) {
    putchar('\n');
    s.row++;
  }

  g_next_off = start_off + (long)i;
  g_next_line = s.line;
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
  if (g_long_status && g_index_full) {
    printf("line %u/%u", (unsigned)g_top_line, (unsigned)g_lines_count);
  } else {
    printf("line %u", (unsigned)g_top_line);
  }
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
  if (g_msg[0]) {
    fputs(C_BYELLOW, stdout);
    fputs(g_msg, stdout);
    fputs(C_RST C_REV, stdout);
  } else {
    fputs(C_DIM, stdout);
    fputs(g_more_mode ? "(q to quit)" : "(/ search · q quit)", stdout);
    fputs(C_RST, stdout);
  }
  fputs("\033[K", stdout);
  fputs(C_RST, stdout);
  fflush(stdout);
}

static void redraw(void) {
  draw_view(g_top_off, g_top_line);
  draw_status();
}

/* ── Search ───────────────────────────────────────────────────────────── */

/* Reads a pattern interactively at the status bar.  `prefix` is the
 * leading character displayed in green ('/' or '?').  Returns the
 * length committed by Enter (0 ⇒ empty pattern → caller treats as
 * cancel); returns -1 if the user pressed ESC / Ctrl-C. */
static int prompt_pattern(char prefix) {
  char buf[MAX_PAT + 1];
  int len = 0;

  /* Switch to a cursor-visible state so the user can see where they're
   * typing; the body of the file is past row g_rows-1 anyway. */
  fputs("\033[?25h", stdout);
  printf("\033[%d;1H\033[K", g_rows);
  fputs(C_BGREEN, stdout);
  putchar(prefix);
  fputs(C_RST, stdout);
  fflush(stdout);

  for (;;) {
    int k = read_key();
    if (k == LKEY_ESC || k == 0x03) {
      fputs("\033[?25l", stdout);
      return -1;
    }
    if (k == '\n' || k == '\r') {
      fputs("\033[?25l", stdout);
      buf[len] = 0;
      if (len > 0) memcpy(g_pattern, buf, (size_t)(len + 1));
      g_pat_len = len;
      return len;
    }
    if (k == 0x7f || k == 0x08) {       /* DEL / backspace */
      if (len > 0) {
        len--;
        fputs("\b \b", stdout);
        fflush(stdout);
      }
      continue;
    }
    /* Only accept printable ASCII into the pattern. */
    if (k >= 0x20 && k < 0x7f && len < MAX_PAT) {
      buf[len++] = (char)k;
      putchar((char)k);
      fflush(stdout);
    }
  }
}

/* Find the next pattern occurrence starting at `from_off` going in
 * `dir` (+1 forward, -1 backward).  Returns the match's byte offset
 * or -1 if none. */
static long find_pattern(long from_off, int dir) {
  if (g_pat_len <= 0 || g_filesize <= 0) return -1;
  if (g_pat_len > (int)sizeof(g_view_buf)) return -1;

  if (dir > 0) {
    if (from_off < 0) from_off = 0;
    long pos = from_off;
    while (pos < g_filesize) {
      lseek(g_fd, pos, 0);
      ssize_t n = read(g_fd, g_view_buf, VIEW_BUFSZ);
      if (n <= 0) return -1;
      for (ssize_t i = 0; i + g_pat_len <= n; i++) {
        int match = 1;
        for (int j = 0; j < g_pat_len; j++) {
          if (g_view_buf[i + j] != g_pattern[j]) { match = 0; break; }
        }
        if (match) return pos + i;
      }
      /* Overlap by pat_len-1 so a match straddling chunks is found. */
      if (n < g_pat_len) return -1;
      pos += n - (g_pat_len - 1);
    }
    return -1;
  }

  /* Backward: scan the whole file in chunks, remember the last match
   * strictly before `from_off`.  O(filesize) but our 4 KB index cap
   * already limits us to files we can stream comfortably. */
  long last = -1;
  long pos = 0;
  while (pos < from_off) {
    lseek(g_fd, pos, 0);
    ssize_t n = read(g_fd, g_view_buf, VIEW_BUFSZ);
    if (n <= 0) break;
    long limit_in_chunk = n;
    if (pos + limit_in_chunk > from_off) limit_in_chunk = from_off - pos;
    for (ssize_t i = 0; i + g_pat_len <= limit_in_chunk; i++) {
      int match = 1;
      for (int j = 0; j < g_pat_len; j++) {
        if (g_view_buf[i + j] != g_pattern[j]) { match = 0; break; }
      }
      if (match) last = pos + i;
    }
    if (n < g_pat_len) break;
    pos += n - (g_pat_len - 1);
  }
  return last;
}

/* Translate a byte offset to its 1-based line number.  Caller must
 * ensure the line is already indexed (we extend up to the offset). */
static long offset_to_line(long off) {
  /* Extend the index until either off is covered or we've indexed
   * everything.  g_scan_off advances monotonically and is always
   * ≥ start of the highest indexed line. */
  while (!g_index_full && g_scan_off <= off && g_lines_count < IDX_MAX) {
    /* Pull one more chunk through idx_extend by asking for the next
     * unindexed line — idx_extend stops at the cap. */
    unsigned before = g_lines_count;
    idx_extend(g_lines_count);
    if (g_lines_count == before) break;  /* no progress → cap or EOF */
  }
  /* Linear scan from the top is fine for IDX_MAX = 4096; a binary
   * search would shave microseconds. */
  long line = 1;
  for (unsigned i = 1; i < g_lines_count; i++) {
    if ((long)g_lines[i] <= off) line = (long)(i + 1);
    else break;
  }
  return line;
}

/* ── Navigation ───────────────────────────────────────────────────────── */

/* Move the viewport so its top is at `target` (1-based line number).
 * Extends the index as needed and clamps to the highest indexed line
 * (which equals the last line in the file once g_index_full is set,
 * or the IDX_MAX cap on huge files). */
static void goto_line(long target) {
  if (target < 1) target = 1;
  /* g_lines[target - 1] holds the offset of `target`; ensure indexed. */
  idx_extend((unsigned)(target - 1));
  if ((unsigned)target > g_lines_count) target = (long)g_lines_count;
  g_top_line = target;
  g_top_off = (long)g_lines[target - 1];
  redraw();
}

static void page_forward(void) {
  if (g_eof) return;
  goto_line(g_top_line + (g_rows - 1));
}

static void page_back(void) {
  goto_line(g_top_line - (g_rows - 1));
}

static void line_forward(void) {
  if (g_eof && g_next_off >= g_filesize) return;
  goto_line(g_top_line + 1);
}

static void line_back(void) {
  if (g_top_line <= 1) return;
  goto_line(g_top_line - 1);
}

static void goto_first(void) {
  goto_line(1);
}

/* `G` — scan the whole file (up to IDX_MAX) and land with the last
 * line at the bottom of the viewport.  On files that exceed IDX_MAX
 * we land at the cap, which is honest about what we can navigate. */
static void goto_last(void) {
  idx_extend((unsigned)-1);     /* fully index */
  long last = (long)g_lines_count;
  long target = last - (g_rows - 2);
  if (target < 1) target = 1;
  goto_line(target);
}

/* Run a search in `dir` (+1 forward, -1 backward).  The starting byte
 * is one past the previous hit when there is one (so `n` advances),
 * or the current viewport top on a fresh search.  Updates g_msg with
 * "Pattern not found" on a miss, otherwise leaves the message empty. */
static void search_jump(int dir) {
  if (g_pat_len <= 0) {
    snprintf(g_msg, sizeof(g_msg), "No previous pattern");
    draw_status();
    return;
  }
  long from;
  if (g_last_match_off < 0) {
    from = g_top_off;
  } else {
    from = (dir > 0) ? g_last_match_off + g_pat_len : g_last_match_off;
  }
  long hit = find_pattern(from, dir);
  if (hit < 0) {
    snprintf(g_msg, sizeof(g_msg), "Pattern not found");
    draw_status();
    return;
  }
  g_last_match_off = hit;
  long line = offset_to_line(hit);
  g_msg[0] = 0;
  goto_line(line);
}

/* ── Help overlay ──────────────────────────────────────────────────────── */

/* Lines printed by the `h` overlay.  Each row is two columns: the key
 * column in cyan and the description in default text.  Lifted to file
 * scope so the renderer doesn't have to walk a per-call string table. */
static const char *const HELP_ROWS[] = {
    "Space, f, PgDn, Down|forward one screen / line",
    "Enter, j|forward one line",
    "b, PgUp|back one screen",
    "k, Up|back one line",
    "g, Home|jump to first line",
    "G, End|jump to last line",
    "/pat, ?pat|search forward / backward (literal)",
    "n, N|repeat search forward / backward",
    "-N|toggle line numbers",
    "-S|toggle chop / wrap long lines",
    "h|this help (any key to dismiss)",
    "q, Ctrl-C|quit",
    NULL,
};

static void show_help(void) {
  fputs("\033[2J\033[H", stdout);
  fputs(C_REV C_BCYAN "  less — help  " C_RST "\n\n", stdout);

  /* Find the widest key column so descriptions line up cleanly. */
  int key_w = 0;
  for (int i = 0; HELP_ROWS[i]; i++) {
    const char *sep = HELP_ROWS[i];
    int n = 0;
    while (*sep && *sep != '|') { sep++; n++; }
    if (n > key_w) key_w = n;
  }

  for (int i = 0; HELP_ROWS[i]; i++) {
    const char *p = HELP_ROWS[i];
    const char *bar = p;
    while (*bar && *bar != '|') bar++;
    int klen = (int)(bar - p);
    fputs("  " C_BCYAN, stdout);
    for (int j = 0; j < klen; j++) putchar(p[j]);
    fputs(C_RST, stdout);
    for (int j = klen; j < key_w + 2; j++) putchar(' ');
    if (*bar == '|') fputs(bar + 1, stdout);
    putchar('\n');
  }

  printf("\n" C_DIM "  press any key to return" C_RST);
  fflush(stdout);

  /* Wait for any keypress, then bounce back to the previous viewport. */
  (void)read_key();
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

    /* Any keystroke clears a sticky status message; search_jump() may
     * set a new one back on miss. */
    g_msg[0] = 0;

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
    } else if (!g_more_mode && (k == 'b' || k == LKEY_PGUP)) {
      page_back();
    } else if (!g_more_mode && (k == 'k' || k == LKEY_UP)) {
      line_back();
    } else if (!g_more_mode && (k == 'g' || k == LKEY_HOME)) {
      goto_first();
    } else if (!g_more_mode && (k == 'G' || k == LKEY_END)) {
      goto_last();
    } else if (!g_more_mode && (k == '/' || k == '?')) {
      int rc = prompt_pattern((char)k);
      if (rc > 0) {
        g_search_dir = (k == '/') ? +1 : -1;
        /* Fresh pattern: anchor the search at the current viewport
         * rather than continuing from a stale prior hit elsewhere. */
        g_last_match_off = -1;
        search_jump(g_search_dir);
      } else {
        /* Cancelled / empty: just redraw to clear the prompt row. */
        draw_status();
      }
    } else if (!g_more_mode && k == 'n') {
      search_jump(g_search_dir ? g_search_dir : +1);
    } else if (!g_more_mode && k == 'N') {
      search_jump(g_search_dir ? -g_search_dir : -1);
    } else if (!g_more_mode && k == '-') {
      /* Two-char toggles: -N, -S.  Read the next key in raw mode. */
      int k2 = read_key();
      if (k2 == 'N') { g_show_lineno = !g_show_lineno; redraw(); }
      else if (k2 == 'S') { g_chop_long = !g_chop_long; redraw(); }
      else { draw_status(); }
    } else if (!g_more_mode && k == 'h') {
      show_help();
    } else {
      /* Unknown key — repaint the status to absorb the cleared msg. */
      draw_status();
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
          "  Interactive pager.  Press `h` inside for the full key list.\n"
          "Environment:\n"
          "  TABS     tab expansion width (1..16, default 8)\n"
          "  LESS     startup flags applied before argv:\n"
          "             -N   show line numbers\n"
          "             -S   chop long lines (default = wrap)\n"
          "             -M   show total line count in the status\n", stdout);
  }
}

/* Apply a single flag character from `LESS` or argv to global state.
 * Unknown letters are silently ignored — matches the proposal's "no
 * whining" stance and keeps the surface easy to extend. */
static void apply_flag(char f) {
  switch (f) {
    case 'N': g_show_lineno = 1; break;
    case 'S': g_chop_long = 1; break;
    case 'M': g_long_status = 1; break;
    default: break;
  }
}

/* Parse the LESS environment variable.  Tokens are either bare letters
 * ("NSM") or dash-prefixed ("-N -S"), separated by whitespace.  We
 * read it byte-by-byte to avoid pulling in strtok. */
static void parse_less_env(void) {
  const char *s = getenv("LESS");
  if (!s) return;
  for (; *s; s++) {
    if (*s == '-' || *s == ' ' || *s == '\t' || *s == ',') continue;
    apply_flag(*s);
  }
}

static void parse_tabs_env(void) {
  const char *s = getenv("TABS");
  if (!s) return;
  int v = 0;
  for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (*s - '0');
  if (v >= 1 && v <= 16) g_tab_width = v;
}

int main(int argc, char *argv[]) {
  g_more_mode = (strcmp(basename_of(argv[0]), "more") == 0);

  /* Env-var defaults first, so command-line flags can still override. */
  parse_tabs_env();
  parse_less_env();

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

  idx_init();
  g_last_match_off = -1;

  int rc = run();

  term_restore();
  fputs("\033[" "0m", stdout);
  printf("\033[%d;1H\n", g_rows);
  fflush(stdout);

  close(g_fd);
  return rc;
}
