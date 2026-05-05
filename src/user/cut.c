/*
 * cut.c — extract fields, characters, or bytes from each line
 *
 * Usage:
 *   cut -f LIST [-d DELIM] [file ...]   extract fields by delimiter
 *   cut -c LIST [file ...]              extract characters
 *   cut -b LIST [file ...]              extract bytes (same as -c
 *                                       under the ASCII-only assumption
 *                                       PPAP makes)
 *
 * LIST is a comma-separated set of ranges:
 *   N         a single position (1-indexed)
 *   N-M       positions N through M
 *   N-        positions N to end of line
 *   -M        positions 1 through M
 *   1,3-5,7   composition
 *
 * Defaults: -d is TAB.  No file (or "-") reads stdin.  A line with
 * no delimiter is printed verbatim under -f.
 *
 * Not implemented: -s (suppress no-delim lines), --complement,
 * --output-delimiter.  Use busybox if you need them.
 *
 * Position bitmap is a 1024-bit array, covering all practical cuts.
 * Open-ended ranges (e.g. "5-") expand to 5..1024; positions beyond
 * 1024 are silently dropped.  Lines longer than 4096 bytes are
 * truncated (a Tier-3 limit; busybox handles arbitrary lines).
 */

#include "lib/uclib.h"

#define MAX_POS 1024
#define LINE_BUF 4096

static unsigned char pos_set[MAX_POS / 8];
static char delim = '\t';

enum cut_mode { MODE_NONE, MODE_BYTES, MODE_CHARS, MODE_FIELDS };

static int is_set(int pos) {
  if (pos < 1 || pos > MAX_POS) return 0;
  return (pos_set[(pos - 1) / 8] >> ((pos - 1) % 8)) & 1;
}

static int parse_list(const char *s) {
  if (!*s) return -1;
  while (*s) {
    int start = 0, end = 0;
    int has_start = 0, has_end = 0;

    while (*s >= '0' && *s <= '9') {
      start = start * 10 + (*s - '0');
      s++;
      has_start = 1;
    }

    if (*s == '-') {
      s++;
      while (*s >= '0' && *s <= '9') {
        end = end * 10 + (*s - '0');
        s++;
        has_end = 1;
      }
      if (!has_start) start = 1;
      if (!has_end) end = MAX_POS;
    } else {
      end = start;
    }

    if (!has_start && !has_end) return -1;
    if (start < 1 || start > MAX_POS) return -1;
    if (end < start) return -1;
    if (end > MAX_POS) end = MAX_POS;

    for (int i = start; i <= end; i++) {
      pos_set[(i - 1) / 8] |= 1 << ((i - 1) % 8);
    }

    if (*s == ',') {
      s++;
      if (!*s) return -1;
    } else if (*s) {
      return -1;
    }
  }
  return 0;
}

static void cut_chars(const char *buf, int n) {
  for (int i = 0; i < n; i++) {
    if (is_set(i + 1)) putchar(buf[i]);
  }
  putchar('\n');
}

static void cut_fields(const char *buf, int n) {
  int has_delim = 0;
  for (int i = 0; i < n; i++) {
    if (buf[i] == delim) {
      has_delim = 1;
      break;
    }
  }
  if (!has_delim) {
    if (n > 0) write(1, buf, n);
    putchar('\n');
    return;
  }

  int field = 1;
  int start = 0;
  int first = 1;
  for (int i = 0; i <= n; i++) {
    if (i == n || buf[i] == delim) {
      if (is_set(field)) {
        if (!first) putchar(delim);
        if (i > start) write(1, buf + start, i - start);
        first = 0;
      }
      start = i + 1;
      field++;
    }
  }
  putchar('\n');
}

static int process_fd(int fd, enum cut_mode mode) {
  char linebuf[LINE_BUF];
  int linelen = 0;
  char buf[256];
  ssize_t n;

  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    for (ssize_t i = 0; i < n; i++) {
      if (buf[i] == '\n') {
        if (mode == MODE_FIELDS) cut_fields(linebuf, linelen);
        else cut_chars(linebuf, linelen);
        linelen = 0;
      } else if (linelen < LINE_BUF) {
        linebuf[linelen++] = buf[i];
      }
      /* lines beyond LINE_BUF are silently truncated */
    }
  }
  if (linelen > 0) {
    if (mode == MODE_FIELDS) cut_fields(linebuf, linelen);
    else cut_chars(linebuf, linelen);
  }
  return (n < 0) ? 1 : 0;
}

static void usage(void) {
  uc_eputs(
      "Usage: cut -f LIST [-d DELIM] [file ...]\n"
      "       cut -c LIST [file ...]\n"
      "       cut -b LIST [file ...]\n"
      "  LIST: N | N-M | N- | -M, comma-separated\n"
      "  -d:   field delimiter (default TAB)\n");
}

int main(int argc, char *argv[]) {
  enum cut_mode mode = MODE_NONE;
  const char *list = 0;
  int argi = 1;

  while (argi < argc && argv[argi][0] == '-' &&
         strcmp(argv[argi], "-") != 0) {
    const char *a = argv[argi];
    if (strcmp(a, "--help") == 0) {
      usage();
      return 0;
    }
    if (strcmp(a, "--") == 0) {
      argi++;
      break;
    }
    if (a[1] == '\0' || a[2] == '\0') {
      /* short option: -X or -X<arg> */
    } else if (a[2] != '\0') {
      /* short option with attached arg, e.g. -f1 or -d, */
    }
    char opt = a[1];
    const char *arg = a[2] ? a + 2 : 0;
    int consumed = 1;
    if (opt == 'f' || opt == 'c' || opt == 'b' || opt == 'd') {
      if (!arg) {
        if (argi + 1 >= argc) {
          uc_eputs("cut: -");
          putchar(opt);
          uc_eputs(" requires an argument\n");
          return 1;
        }
        arg = argv[argi + 1];
        consumed = 2;
      }
    }
    switch (opt) {
      case 'f':
        if (mode != MODE_NONE) {
          uc_eputs("cut: only one of -f, -c, -b allowed\n");
          return 1;
        }
        mode = MODE_FIELDS;
        list = arg;
        break;
      case 'c':
        if (mode != MODE_NONE) {
          uc_eputs("cut: only one of -f, -c, -b allowed\n");
          return 1;
        }
        mode = MODE_CHARS;
        list = arg;
        break;
      case 'b':
        if (mode != MODE_NONE) {
          uc_eputs("cut: only one of -f, -c, -b allowed\n");
          return 1;
        }
        mode = MODE_BYTES;
        list = arg;
        break;
      case 'd':
        if (!arg[0] || arg[1]) {
          uc_eputs("cut: -d takes a single character\n");
          return 1;
        }
        delim = arg[0];
        break;
      default:
        uc_eputs("cut: unknown option: ");
        uc_eputs(a);
        uc_eputs("\n");
        return 1;
    }
    argi += consumed;
  }

  if (mode == MODE_NONE || !list) {
    usage();
    return 1;
  }
  if (parse_list(list) < 0) {
    uc_eputs("cut: invalid LIST: ");
    uc_eputs(list);
    uc_eputs("\n");
    return 1;
  }

  if (argi >= argc) return process_fd(0, mode);

  int rc = 0;
  for (int i = argi; i < argc; i++) {
    int fd;
    if (strcmp(argv[i], "-") == 0) {
      fd = 0;
    } else {
      fd = open(argv[i], O_RDONLY, 0);
      if (fd < 0) {
        uc_eputs("cut: ");
        uc_eputs(argv[i]);
        uc_eputs(": No such file or directory\n");
        rc = 1;
        continue;
      }
    }
    if (process_fd(fd, mode)) rc = 1;
    if (fd != 0) close(fd);
  }
  return rc;
}
