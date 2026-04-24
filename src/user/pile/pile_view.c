/*
 * pile_view.c — Hex and text viewers for the pile filer
 *
 * User guide: docs/user/pile.md
 */

#include "pile.h"

#include "common/errno.h"

#define VIEW_CHROME_ROWS 3   /* title + rule + legend */
#define VIEW_READ_BUF    1024
#define VIEW_DETECT_BYTES 2048

#define V_C(seq) (pile_use_color ? (seq) : "")
#define V_RST    V_C("\033[0m")
#define V_FRAME  V_C("\033[2m")
#define V_HEADER V_C("\033[1m")
#define V_KEY    V_C("\033[1m")

static int vfd = -1;
static uint32_t vsize;
static uint32_t voff;            /* byte offset of the first visible row */
static int vbytes_per_row;       /* 16 or 8 */
static int voff_width;           /* 4, 6, or 8 hex digits */
static int vrows_visible;
static int vshow_ascii;          /* include ASCII gutter? */
static uint8_t vbuf[VIEW_READ_BUF];

/* Text viewer state.  No fixed-size content buffer — the viewer is
 * streaming: each render lseeks to the start of the file, skips past
 * tscroll lines, and reads / displays one screenful.  tlines is
 * computed once at load time by scanning the whole file. */
static int tlines;
static int tscroll;              /* first visible line index */

/* ── Small utilities (write-style, not printf-like) ───────────────────── */

static void vemit(const char *s) {
  int n = 0;
  while (s[n]) n++;
  if (n > 0) write(1, s, n);
}

static void vput_hex(uint32_t v, int digits) {
  char buf[9];
  static const char hx[] = "0123456789abcdef";
  if (digits < 1) digits = 1;
  if (digits > 8) digits = 8;
  for (int i = digits - 1; i >= 0; i--) {
    buf[i] = hx[v & 0xF];
    v >>= 4;
  }
  buf[digits] = '\0';
  vemit(buf);
}

static int hex_digit_val(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return -1;
}

/* ── Layout ───────────────────────────────────────────────────────────── */

static void compute_layout(void) {
  /* 16 bytes per row needs ~75 cols worth of chrome for the row line
   * (8-digit offset + 2 spaces + 48 bytes of hex + 2 spaces + 18 ASCII).
   * Threshold conservatively at 74; narrower falls back to 8 bytes. */
  if (pile_cols >= 74) {
    vbytes_per_row = 16;
    vshow_ascii = 1;
  } else if (pile_cols >= 45) {
    vbytes_per_row = 8;
    vshow_ascii = 1;
  } else {
    vbytes_per_row = 8;
    vshow_ascii = 0;
  }

  if (vsize <= 0xFFFFu) voff_width = 4;
  else if (vsize <= 0xFFFFFFu) voff_width = 6;
  else voff_width = 8;

  vrows_visible = pile_rows - VIEW_CHROME_ROWS;
  if (vrows_visible < 1) vrows_visible = 1;
}

static uint32_t page_bytes(void) {
  return (uint32_t)vbytes_per_row * (uint32_t)vrows_visible;
}

static void clamp_offset(void) {
  /* Round down to a row boundary. */
  voff = (voff / (uint32_t)vbytes_per_row) * (uint32_t)vbytes_per_row;
  uint32_t page = page_bytes();
  if (vsize <= page) {
    voff = 0;
  } else if (voff > vsize - page) {
    /* Anchor the last full page to a row boundary. */
    uint32_t last = vsize - page;
    voff = (last / (uint32_t)vbytes_per_row) * (uint32_t)vbytes_per_row;
  }
}

/* ── Rendering ────────────────────────────────────────────────────────── */

static const char *basename_of(const char *path) {
  const char *slash = uc_strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static void draw_title(const char *path) {
  pile_draw_cursor_to(0, 0);
  vemit(V_HEADER);
  vemit("-- ");
  const char *b = basename_of(path);
  int blen = uc_strlen(b);
  int max_b = pile_cols / 2;
  if (max_b < 4) max_b = 4;
  int shown = blen < max_b ? blen : max_b;
  for (int i = 0; i < shown; i++) uc_putc(b[i]);
  if (blen > shown) uc_putc('~');
  vemit(V_RST);

  vemit(V_FRAME);
  vemit("  offset ");
  vput_hex(voff, voff_width);
  vemit(" / ");
  vput_hex(vsize, voff_width);
  vemit(" ");
  vemit(V_RST);
  pile_draw_clear_to_eol();
}

static void draw_hex_row(int row, uint32_t offset, const uint8_t *data,
                         int len) {
  pile_draw_cursor_to(row, 0);

  vemit(V_FRAME);
  vput_hex(offset, voff_width);
  vemit(V_RST);
  uc_putc(' ');
  uc_putc(' ');

  int half = vbytes_per_row / 2;
  for (int i = 0; i < vbytes_per_row; i++) {
    if (i == half) uc_putc(' ');
    if (i < len) {
      vput_hex(data[i], 2);
    } else {
      uc_putc(' ');
      uc_putc(' ');
    }
    if (i + 1 < vbytes_per_row) uc_putc(' ');
  }

  if (vshow_ascii) {
    uc_putc(' ');
    uc_putc(' ');
    vemit(V_FRAME);
    uc_putc('|');
    vemit(V_RST);
    for (int i = 0; i < vbytes_per_row; i++) {
      if (i < len) {
        uint8_t b = data[i];
        uc_putc((b >= 32 && b < 127) ? (char)b : '.');
      } else {
        uc_putc(' ');
      }
    }
    vemit(V_FRAME);
    uc_putc('|');
    vemit(V_RST);
  }

  pile_draw_clear_to_eol();
}

static void draw_rule(int row) {
  pile_draw_cursor_to(row, 0);
  vemit(V_FRAME);
  for (int i = 0; i < pile_cols; i++) uc_putc('-');
  vemit(V_RST);
}

static void draw_legend(int row) {
  pile_draw_cursor_to(row, 0);
  uc_putc(' ');
  vemit(V_KEY); vemit("PgUp"); vemit(V_RST); uc_putc('/');
  vemit(V_KEY); vemit("PgDn"); vemit(V_RST); vemit(" page  ");
  vemit(V_KEY); vemit("g");    vemit(V_RST); uc_putc('/');
  vemit(V_KEY); vemit("G");    vemit(V_RST); vemit(" start/end  ");
  vemit(V_KEY); vemit(":");    vemit(V_RST); vemit(" goto  ");
  vemit(V_KEY); vemit("q");    vemit(V_RST); vemit(" quit");
  pile_draw_clear_to_eol();
}

/* Read exactly one page of bytes starting at voff.  On short reads
 * near EOF, remaining slots in vbuf are left untouched and the caller
 * relies on the returned length. */
static int read_page(void) {
  if (lseek(vfd, (int)voff, 0) < 0) return 0;
  uint32_t want = page_bytes();
  if (want > sizeof(vbuf)) want = sizeof(vbuf);
  int got = 0;
  while ((uint32_t)got < want) {
    int n = (int)read(vfd, vbuf + got, want - (uint32_t)got);
    if (n <= 0) break;
    got += n;
  }
  return got;
}

static void render(const char *path) {
  draw_title(path);

  int got = read_page();
  for (int i = 0; i < vrows_visible; i++) {
    uint32_t row_off = voff + (uint32_t)(i * vbytes_per_row);
    int base = i * vbytes_per_row;
    int len = got - base;
    if (len < 0) len = 0;
    if (len > vbytes_per_row) len = vbytes_per_row;
    if (row_off >= vsize && len == 0) {
      /* Blank the remaining rows past EOF so stale content clears. */
      pile_draw_cursor_to(1 + i, 0);
      pile_draw_clear_to_eol();
      continue;
    }
    draw_hex_row(1 + i, row_off, vbuf + base, len);
  }

  draw_rule(pile_rows - 2);
  draw_legend(pile_rows - 1);
  /* Don't park at the bottom-right corner — some terminals set a
   * pending-wrap latch that scrolls the screen on the next write. */
  pile_draw_cursor_to(pile_rows - 1, 0);
}

/* ── Text viewer (streaming) ──────────────────────────────────────────── */

/* Scan the whole file once via the shared vbuf to count lines.  An
 * incomplete final line (no trailing newline) still counts; an empty
 * file reports 1 so scroll math stays trivial. */
static int text_scan_lines(int fd) {
  if (lseek(fd, 0, 0) < 0) return 1;
  int n = 1;
  int last = 0;
  int got_any = 0;
  for (;;) {
    int r = (int)read(fd, vbuf, sizeof(vbuf));
    if (r <= 0) break;
    got_any = 1;
    for (int i = 0; i < r; i++) {
      last = vbuf[i];
      if (last == '\n') n++;
    }
  }
  if (!got_any) return 1;
  /* A file ending with '\n' has N lines, not N+1. */
  if (last == '\n' && n > 1) n--;
  return n;
}

/* Sniff first VIEW_DETECT_BYTES bytes for >=95% printable ASCII +
 * \t / \n / \r.  Empty files count as text. */
static int looks_like_text(int fd, uint32_t size) {
  if (size == 0) return 1;
  if (lseek(fd, 0, 0) < 0) return 0;
  uint32_t want = size < VIEW_DETECT_BYTES ? size : VIEW_DETECT_BYTES;
  int total = 0;
  int printable = 0;
  while ((uint32_t)total < want) {
    int r = (int)read(fd, vbuf, sizeof(vbuf));
    if (r <= 0) break;
    uint32_t take = (uint32_t)r;
    if ((uint32_t)total + take > want) take = want - (uint32_t)total;
    for (uint32_t i = 0; i < take; i++) {
      uint8_t b = vbuf[i];
      if ((b >= 32 && b < 127) || b == '\t' || b == '\n' || b == '\r')
        printable++;
    }
    total += (int)take;
    if ((uint32_t)r > take) break;
  }
  if (total == 0) return 1;
  return (printable * 100) / total >= 95;
}

static void draw_text_title(const char *path) {
  pile_draw_cursor_to(0, 0);
  vemit(V_HEADER);
  vemit("-- ");
  const char *b = basename_of(path);
  int blen = uc_strlen(b);
  int max_b = pile_cols / 2;
  if (max_b < 4) max_b = 4;
  int shown = blen < max_b ? blen : max_b;
  for (int i = 0; i < shown; i++) uc_putc(b[i]);
  if (blen > shown) uc_putc('~');
  vemit(V_RST);

  vemit(V_FRAME);
  vemit("  line ");
  /* Cheap put_uint. */
  char numbuf[12];
  int pos = (int)sizeof(numbuf);
  numbuf[--pos] = '\0';
  int n = tscroll + 1;
  if (n == 0) numbuf[--pos] = '0';
  while (n > 0 && pos > 0) {
    numbuf[--pos] = (char)('0' + n % 10);
    n /= 10;
  }
  vemit(numbuf + pos);
  uc_putc('/');
  pos = (int)sizeof(numbuf);
  numbuf[--pos] = '\0';
  n = tlines;
  if (n == 0) numbuf[--pos] = '0';
  while (n > 0 && pos > 0) {
    numbuf[--pos] = (char)('0' + n % 10);
    n /= 10;
  }
  vemit(numbuf + pos);
  uc_putc(' ');
  vemit(V_RST);
  pile_draw_clear_to_eol();
}

static void draw_text_legend(int row) {
  pile_draw_cursor_to(row, 0);
  uc_putc(' ');
  vemit(V_KEY); vemit("PgUp"); vemit(V_RST); uc_putc('/');
  vemit(V_KEY); vemit("PgDn"); vemit(V_RST); vemit(" page  ");
  vemit(V_KEY); vemit("g");    vemit(V_RST); uc_putc('/');
  vemit(V_KEY); vemit("G");    vemit(V_RST); vemit(" start/end  ");
  vemit(V_KEY); vemit("V");    vemit(V_RST); vemit(" hex  ");
  vemit(V_KEY); vemit("q");    vemit(V_RST); vemit(" quit");
  pile_draw_clear_to_eol();
}

/* Streaming text renderer.  Re-reads the file on every render: no
 * in-memory content buffer means no per-file size cap and no BSS
 * footprint beyond the shared vbuf.  O(tscroll + vrows * pile_cols)
 * per render; acceptable for interactive use on the tight ia16
 * segment budget. */
static void render_text(const char *path) {
  draw_text_title(path);

  if (lseek(vfd, 0, 0) < 0) {
    pile_draw_cursor_to(1, 0);
    pile_draw_clear_to_eol();
    return;
  }

  /* vbuf is the shared chunked-read buffer.  buf_len == -1 signals
   * "no more bytes" so the render loop can short-circuit remaining
   * rows. */
  int buf_pos = 0;
  int buf_len = 0;

  /* Skip past tscroll newlines. */
  int skipped = 0;
  while (skipped < tscroll) {
    if (buf_pos >= buf_len) {
      buf_len = (int)read(vfd, vbuf, sizeof(vbuf));
      buf_pos = 0;
      if (buf_len <= 0) break;
    }
    if (vbuf[buf_pos++] == '\n') skipped++;
  }

  /* Render one row at a time, reading one line from the chunk stream.
   * Only conclude EOF here if the skip loop actually tried to read
   * (tscroll > 0) and came up short; when tscroll == 0 the buffer is
   * still virgin and the inner loop's own read will determine EOF. */
  int eof = (tscroll > 0 && buf_len <= 0);
  for (int row = 0; row < vrows_visible; row++) {
    pile_draw_cursor_to(1 + row, 0);
    if (eof) {
      pile_draw_clear_to_eol();
      continue;
    }
    int col = 0;
    int line_end = 0;
    while (col < pile_cols && !line_end) {
      if (buf_pos >= buf_len) {
        buf_len = (int)read(vfd, vbuf, sizeof(vbuf));
        buf_pos = 0;
        if (buf_len <= 0) {
          eof = 1;
          line_end = 1;
          break;
        }
      }
      unsigned char c = vbuf[buf_pos++];
      if (c == '\n') {
        line_end = 1;
        break;
      }
      if (c == '\t') {
        int spaces = 8 - (col % 8);
        for (int j = 0; j < spaces && col < pile_cols; j++) {
          uc_putc(' ');
          col++;
        }
      } else if (c == '\r') {
        /* swallow bare CR so CRLF files read cleanly */
      } else if (c >= 32 && c < 127) {
        uc_putc((char)c);
        col++;
      } else {
        uc_putc('.');
        col++;
      }
    }
    /* If the visible width was reached mid-line, drain the rest of
     * the line so the next row starts at the next logical line. */
    while (!line_end) {
      if (buf_pos >= buf_len) {
        buf_len = (int)read(vfd, vbuf, sizeof(vbuf));
        buf_pos = 0;
        if (buf_len <= 0) {
          eof = 1;
          break;
        }
      }
      if (vbuf[buf_pos++] == '\n') line_end = 1;
    }
    pile_draw_clear_to_eol();
  }

  draw_rule(pile_rows - 2);
  draw_text_legend(pile_rows - 1);
  /* Don't park at the bottom-right corner — some terminals set a
   * pending-wrap latch that scrolls the screen on the next write. */
  pile_draw_cursor_to(pile_rows - 1, 0);
}

/* ── :offset prompt ───────────────────────────────────────────────────── */

static int goto_prompt(void) {
  char buf[16];
  if (pile_prompt(":", buf, (int)sizeof(buf)) != 0) return 0;
  const char *s = buf;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  if (!*s) return 0;

  uint32_t v = 0;
  while (*s) {
    int d = hex_digit_val((unsigned char)*s);
    if (d < 0) {
      pile_status_set("pile: invalid hex offset", 1);
      return 0;
    }
    v = (v << 4) | (uint32_t)d;
    s++;
  }
  voff = v;
  clamp_offset();
  return 1;
}

/* ── Hex mode loop ────────────────────────────────────────────────────── */

/* Returns 1 if the user pressed V (toggle to hex) from text mode, or
 * any "quit" key to exit the viewer entirely. */
static void run_hex_loop(const char *path) {
  for (;;) {
    render(path);
    int k = pile_read_key();
    if (k == PKEY_NONE) continue;

    int step = vbytes_per_row;
    uint32_t page = page_bytes();

    switch (k) {
      case 'q':
      case PKEY_ESC:
        return;
      case PKEY_UP:
      case 'k':
        if (voff >= (uint32_t)step) voff -= (uint32_t)step;
        else voff = 0;
        break;
      case PKEY_DOWN:
      case 'j':
        voff += (uint32_t)step;
        break;
      case PKEY_PGUP:
        if (voff >= page) voff -= page;
        else voff = 0;
        break;
      case PKEY_PGDN:
        voff += page;
        break;
      case PKEY_HOME:
      case 'g':
        voff = 0;
        break;
      case PKEY_END:
      case 'G':
        voff = vsize;
        break;
      case ':':
        goto_prompt();
        break;
    }
    clamp_offset();
  }
}

static void run_text_loop(const char *path) {
  for (;;) {
    render_text(path);
    int k = pile_read_key();
    if (k == PKEY_NONE) continue;

    int last_scroll = tlines - vrows_visible;
    if (last_scroll < 0) last_scroll = 0;

    switch (k) {
      case 'q':
      case PKEY_ESC:
        return;
      case PKEY_UP:
      case 'k':
        if (tscroll > 0) tscroll--;
        break;
      case PKEY_DOWN:
      case 'j':
        if (tscroll < last_scroll) tscroll++;
        break;
      case PKEY_PGUP:
        tscroll -= vrows_visible;
        if (tscroll < 0) tscroll = 0;
        break;
      case PKEY_PGDN:
        tscroll += vrows_visible;
        if (tscroll > last_scroll) tscroll = last_scroll;
        break;
      case PKEY_HOME:
      case 'g':
        tscroll = 0;
        break;
      case PKEY_END:
      case 'G':
        tscroll = last_scroll;
        break;
    }
  }
}

/* ── Entry point ──────────────────────────────────────────────────────── */

void pile_view_file(const char *path, int force_hex) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    pile_status_set_errno("view", fd);
    return;
  }
  struct stat st;
  if (stat(path, &st) != 0) {
    close(fd);
    pile_status_set("view: stat failed", 1);
    return;
  }
  vfd = fd;
  vsize = (uint32_t)st.st_size;
  voff = 0;
  tscroll = 0;
  compute_layout();

  int use_text = !force_hex && looks_like_text(fd, vsize);
  if (use_text) {
    tlines = text_scan_lines(fd);
  }

  pile_draw_clear();

  if (use_text) {
    run_text_loop(path);
  } else {
    run_hex_loop(path);
  }

  close(vfd);
  vfd = -1;
  pile_draw_clear();
}
