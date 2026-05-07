/*
 * head.c — print first lines / bytes of files
 *
 * Usage: head [-n N | -c N] [file ...]
 *   -n N  print first N lines (default 10)
 *   -c N  print first N bytes
 * With no file (or "-"), reads stdin.
 * Multiple files print "==> name <==" headers separated by blank lines.
 */

#include "lib/uclib.h"

static uint32_t limit = 10;
static int byte_mode;

/* Stream up to `limit` lines or bytes from fd to stdout.  Stops as soon
 * as the limit is hit so subsequent files / commands aren't blocked.
 * Returns 1 on read error, 0 otherwise. */
static int head_fd(int fd) {
  char buf[256];
  uint32_t emitted = 0;
  ssize_t n = 0;

  while (emitted < limit && (n = read(fd, buf, sizeof(buf))) > 0) {
    ssize_t take = n;
    if (byte_mode) {
      uint32_t room = limit - emitted;
      if ((uint32_t)take > room) take = (ssize_t)room;
      emitted += (uint32_t)take;
    } else {
      ssize_t i = 0;
      for (; i < n && emitted < limit; i++) {
        if (buf[i] == '\n') emitted++;
      }
      take = i;
    }
    ssize_t off = 0;
    while (off < take) {
      ssize_t w = write(1, buf + off, take - off);
      if (w <= 0) return 1;
      off += w;
    }
  }
  return (n < 0) ? 1 : 0;
}

/* Parse "-n N", "-c N", "-nN", "-cN", "--help", or "--".  Returns the
 * number of argv slots consumed (1 or 2), or -1 on error. */
static int parse_flag(int argc, char *argv[], int argi) {
  const char *a = argv[argi];
  if (strcmp(a, "--help") == 0) {
    fputs(
        "Usage: head [-n N | -c N] [file ...]\n"
        "  -n N  Print first N lines (default 10)\n"
        "  -c N  Print first N bytes\n"
        "  -     Read from stdin\n", stdout);
    return 0;
  }
  if (strcmp(a, "--") == 0) return 1;
  if (a[1] != 'n' && a[1] != 'c') {
    fputs("head: unknown option: ", stderr);
    fputs(a, stderr);
    fputs("\n", stderr);
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
      fputs("head: -", stderr);
      putchar(a[1]);
      fputs(" requires a count\n", stderr);
      return -1;
    }
    num = argv[argi + 1];
    consumed = 2;
  }
  uint32_t v;
  if (uc_parse_u32(num, &v) != 0) {
    fputs("head: invalid count: ", stderr);
    fputs(num, stderr);
    fputs("\n", stderr);
    return -1;
  }
  limit = v;
  return consumed;
}

int main(int argc, char *argv[]) {
  int argi = 1;

  while (argi < argc && argv[argi][0] == '-' &&
         strcmp(argv[argi], "-") != 0) {
    int consumed = parse_flag(argc, argv, argi);
    if (consumed < 0) return 1;
    if (consumed == 0) return 0;
    if (strcmp(argv[argi], "--") == 0) {
      argi += consumed;
      break;
    }
    argi += consumed;
  }

  if (argi >= argc) return head_fd(0);

  int rc = 0;
  int multi = (argc - argi) > 1;
  int first = 1;

  for (int i = argi; i < argc; i++) {
    int fd;
    if (strcmp(argv[i], "-") == 0) {
      fd = 0;
    } else {
      fd = open(argv[i], O_RDONLY, 0);
      if (fd < 0) {
        fputs("head: ", stderr);
        fputs(argv[i], stderr);
        fputs(": No such file or directory\n", stderr);
        rc = 1;
        continue;
      }
    }
    if (multi) {
      if (!first) putchar('\n');
      fputs("==> ", stdout);
      fputs(argv[i], stdout);
      fputs(" <==\n", stdout);
      first = 0;
    }
    if (head_fd(fd)) rc = 1;
    if (fd != 0) close(fd);
  }
  return rc;
}
