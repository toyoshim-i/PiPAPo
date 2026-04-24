/*
 * pile_view.c — Hex and text viewers for the pile filer
 *
 * Design: docs/proposals/pile.md Phase P4
 *
 * P4a lands the hex viewer and the shared open / keyboard loop.
 * Text viewer + auto-detection (sniff first 4 KB for > 95% printable)
 * land in P4b.
 */

#include "pile.h"

#include "common/errno.h"

#define VIEW_CHROME_ROWS 3   /* title + rule + legend */
#define VIEW_READ_BUF    1024

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
  pile_draw_cursor_to(pile_rows - 1, pile_cols - 1);
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

/* ── Entry point ──────────────────────────────────────────────────────── */

void pile_view_file(const char *path, int force_hex) {
  (void)force_hex;  /* P4a: always hex; auto-detect lands in P4b */

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
  compute_layout();

  pile_draw_clear();  /* wipe filer chrome before taking over the screen */

  for (;;) {
    render(path);
    int k = pile_read_key();
    if (k == PKEY_NONE) continue;

    int step = vbytes_per_row;
    uint32_t page = page_bytes();

    switch (k) {
      case 'q':
      case PKEY_ESC:
      case PKEY_F10:
      case PKEY_F3:
        goto done;
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

done:
  close(vfd);
  vfd = -1;
  pile_draw_clear();
}
