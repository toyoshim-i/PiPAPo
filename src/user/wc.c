/*
 * wc.c — count lines, words, and bytes
 *
 * Usage: wc [-lwc] [file ...]
 *   -l  count newlines
 *   -w  count whitespace-separated words
 *   -c  count bytes
 * With no flags, all three are printed.
 * With no file (or "-"), reads stdin.
 * Multiple files print a "total" line at the end.
 */

#include "lib/uclib.h"

#define F_L 0x01
#define F_W 0x02
#define F_C 0x04
#define F_ALL (F_L | F_W | F_C)

typedef struct {
  uint32_t lines;
  uint32_t words;
  uint32_t bytes;
} wc_count_t;

static int flags;

static int is_ws(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
         c == '\v' || c == '\f';
}

static int wc_fd(int fd, wc_count_t *out) {
  char buf[256];
  ssize_t n;
  int in_word = 0;

  out->lines = 0;
  out->words = 0;
  out->bytes = 0;

  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    out->bytes += (uint32_t)n;
    for (ssize_t i = 0; i < n; i++) {
      unsigned char c = (unsigned char)buf[i];
      if (c == '\n') out->lines++;
      if (is_ws(c)) {
        in_word = 0;
      } else if (!in_word) {
        in_word = 1;
        out->words++;
      }
    }
  }
  return (n < 0) ? 1 : 0;
}

static void print_field(uint32_t v, int *first) {
  char tmp[12];
  if (!*first) uc_putc(' ');
  uc_snprintf(tmp, (int)sizeof(tmp), "%7u", v);
  uc_puts(tmp);
  *first = 0;
}

static void print_counts(const wc_count_t *c, const char *name) {
  int first = 1;
  if (flags & F_L) print_field(c->lines, &first);
  if (flags & F_W) print_field(c->words, &first);
  if (flags & F_C) print_field(c->bytes, &first);
  if (name) {
    uc_putc(' ');
    uc_puts(name);
  }
  uc_putc('\n');
}

int main(int argc, char *argv[]) {
  int argi = 1;

  while (argi < argc && argv[argi][0] == '-' &&
         uc_strcmp(argv[argi], "-") != 0) {
    if (uc_strcmp(argv[argi], "--help") == 0) {
      uc_puts(
          "Usage: wc [-lwc] [file ...]\n"
          "  -l  Count newlines\n"
          "  -w  Count words\n"
          "  -c  Count bytes\n"
          "  -   Read from stdin\n"
          "Default with no flags: -lwc\n");
      return 0;
    }
    const char *p = argv[argi] + 1;
    while (*p) {
      switch (*p) {
        case 'l': flags |= F_L; break;
        case 'w': flags |= F_W; break;
        case 'c': flags |= F_C; break;
        default:
          uc_eputs("wc: unknown option: -");
          uc_putc(*p);
          uc_eputs("\n");
          return 1;
      }
      p++;
    }
    argi++;
  }

  if (flags == 0) flags = F_ALL;

  if (argi >= argc) {
    wc_count_t c;
    int rc = wc_fd(0, &c);
    print_counts(&c, NULL);
    return rc;
  }

  int rc = 0;
  wc_count_t total = {0, 0, 0};
  int multi = (argc - argi) > 1;

  for (int i = argi; i < argc; i++) {
    int fd;
    if (uc_strcmp(argv[i], "-") == 0) {
      fd = 0;
    } else {
      fd = open(argv[i], O_RDONLY, 0);
      if (fd < 0) {
        uc_eputs("wc: ");
        uc_eputs(argv[i]);
        uc_eputs(": No such file or directory\n");
        rc = 1;
        continue;
      }
    }
    wc_count_t c;
    if (wc_fd(fd, &c)) rc = 1;
    print_counts(&c, argv[i]);
    total.lines += c.lines;
    total.words += c.words;
    total.bytes += c.bytes;
    if (fd != 0) close(fd);
  }

  if (multi) print_counts(&total, "total");
  return rc;
}
