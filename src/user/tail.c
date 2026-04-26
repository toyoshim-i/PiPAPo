/*
 * tail.c — print last lines / bytes of files
 *
 * Usage: tail [-n N | -c N] file ...
 *   -n N  print last N lines (default 10)
 *   -c N  print last N bytes
 * Multiple files print "==> name <==" headers separated by blank lines.
 *
 * v1: file-only.  Uses lseek to walk backward from EOF — no large buffer.
 * Stdin support is deferred until we add either uc_getline or a uc_malloc
 * ring buffer.
 */

#include "lib/uclib.h"

static uint32_t limit = 10;
static int byte_mode;

/* Copy fd bytes [start, end_of_file) to stdout.  Returns 1 on read/write
 * error, 0 otherwise. */
static int copy_from(int fd, int start) {
  if (lseek(fd, start, SEEK_SET) < 0) return 1;
  char buf[256];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    ssize_t off = 0;
    while (off < n) {
      ssize_t w = write(1, buf + off, n - off);
      if (w <= 0) return 1;
      off += w;
    }
  }
  return (n < 0) ? 1 : 0;
}

/* Locate the byte offset where the last `lines` lines begin.  Returns 0
 * (start of file) if the file has fewer than `lines` lines.  A trailing
 * newline is treated as the terminator of the last line, not as the
 * start of a new empty one. */
static int find_tail_offset(int fd, int size, uint32_t lines) {
  if (size <= 0 || lines == 0) return size;
  char buf[512];
  int pos = size;
  uint32_t found = 0;
  int first_chunk = 1;

  while (pos > 0 && found < lines) {
    int chunk = (pos >= (int)sizeof(buf)) ? (int)sizeof(buf) : pos;
    pos -= chunk;
    if (lseek(fd, pos, SEEK_SET) < 0) return -1;
    if (read(fd, buf, chunk) != chunk) return -1;

    int i_start = chunk - 1;
    if (first_chunk && buf[i_start] == '\n') i_start--;
    first_chunk = 0;

    for (int i = i_start; i >= 0; i--) {
      if (buf[i] == '\n') {
        found++;
        if (found == lines) return pos + i + 1;
      }
    }
  }
  return 0;
}

static int tail_file(int fd) {
  int size = lseek(fd, 0, SEEK_END);
  if (size < 0) {
    uc_eputs("tail: lseek failed (non-seekable input)\n");
    return 1;
  }
  if (size == 0) return 0;

  int start;
  if (byte_mode) {
    start = ((uint32_t)size > limit) ? size - (int)limit : 0;
  } else {
    start = find_tail_offset(fd, size, limit);
    if (start < 0) return 1;
  }
  return copy_from(fd, start);
}

static int parse_flag(int argc, char *argv[], int argi) {
  const char *a = argv[argi];
  if (uc_strcmp(a, "--help") == 0) {
    uc_puts(
        "Usage: tail [-n N | -c N] file ...\n"
        "  -n N  Print last N lines (default 10)\n"
        "  -c N  Print last N bytes\n"
        "Stdin not yet supported.\n");
    return 0;
  }
  if (uc_strcmp(a, "--") == 0) return 1;
  if (a[1] != 'n' && a[1] != 'c') {
    uc_eputs("tail: unknown option: ");
    uc_eputs(a);
    uc_eputs("\n");
    return -1;
  }
  byte_mode = (a[1] == 'c');
  const char *num;
  int consumed;
  if (a[2] != '\0') {
    num = a + 2;
    consumed = 1;
  } else {
    if (argi + 1 >= argc) {
      uc_eputs("tail: -");
      uc_putc(a[1]);
      uc_eputs(" requires a count\n");
      return -1;
    }
    num = argv[argi + 1];
    consumed = 2;
  }
  uint32_t v;
  if (uc_parse_u32(num, &v) != 0) {
    uc_eputs("tail: invalid count: ");
    uc_eputs(num);
    uc_eputs("\n");
    return -1;
  }
  limit = v;
  return consumed;
}

int main(int argc, char *argv[]) {
  int argi = 1;

  while (argi < argc && argv[argi][0] == '-' &&
         uc_strcmp(argv[argi], "-") != 0) {
    int consumed = parse_flag(argc, argv, argi);
    if (consumed < 0) return 1;
    if (consumed == 0) return 0;
    if (uc_strcmp(argv[argi], "--") == 0) {
      argi += consumed;
      break;
    }
    argi += consumed;
  }

  if (argi >= argc) {
    uc_eputs("tail: stdin not yet supported\n");
    return 1;
  }

  int rc = 0;
  int multi = (argc - argi) > 1;
  int first = 1;

  for (int i = argi; i < argc; i++) {
    if (uc_strcmp(argv[i], "-") == 0) {
      uc_eputs("tail: '-' (stdin) not yet supported\n");
      rc = 1;
      continue;
    }
    int fd = open(argv[i], O_RDONLY, 0);
    if (fd < 0) {
      uc_eputs("tail: ");
      uc_eputs(argv[i]);
      uc_eputs(": No such file or directory\n");
      rc = 1;
      continue;
    }
    if (multi) {
      if (!first) uc_putc('\n');
      uc_puts("==> ");
      uc_puts(argv[i]);
      uc_puts(" <==\n");
      first = 0;
    }
    if (tail_file(fd)) rc = 1;
    close(fd);
  }
  return rc;
}
