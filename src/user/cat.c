/*
 * cat.c — concatenate and print files
 *
 * Usage: cat [-nE] [file ...]
 * With no arguments, copies stdin to stdout.
 * -n: number all output lines.
 * -E: display $ at end of each line.
 */

#include "lib/uclib.h"

static int opt_number;
static int opt_show_ends;
static int line_nr;

static int cat_fd(int fd) {
  char buf[256];
  ssize_t n;
  int at_bol = 1; /* at beginning of line */

  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    for (ssize_t i = 0; i < n; i++) {
      if (opt_number && at_bol) printf("%6d\t", ++line_nr);
      if (buf[i] == '\n') {
        if (opt_show_ends) putchar('$');
        putchar('\n');
        at_bol = 1;
      } else {
        putchar(buf[i]);
        at_bol = 0;
      }
    }
  }
  return (n < 0) ? 1 : 0;
}

int main(int argc, char *argv[]) {
  int argi = 1;

  while (argi < argc && argv[argi][0] == '-' &&
         strcmp(argv[argi], "-") != 0) {
    if (strcmp(argv[argi], "--help") == 0) {
      fputs(
          "Usage: cat [-nE] [file ...]\n"
          "  -n  Number output lines\n"
          "  -E  Show $ at end of each line\n"
          "  -   Read from stdin\n", stdout);
      return 0;
    }
    const char *p = argv[argi] + 1;
    while (*p) {
      switch (*p) {
        case 'n':
          opt_number = 1;
          break;
        case 'E':
          opt_show_ends = 1;
          break;
        default:
          fputs("cat: unknown option: -", stderr);
          putchar(*p);
          fputs("\n", stderr);
          return 1;
      }
      p++;
    }
    argi++;
  }

  if (argi >= argc) return cat_fd(0);

  int rc = 0;
  for (int i = argi; i < argc; i++) {
    int fd;
    if (strcmp(argv[i], "-") == 0) {
      fd = 0;
    } else {
      fd = open(argv[i], O_RDONLY, 0);
      if (fd < 0) {
        fputs("cat: ", stderr);
        fputs(argv[i], stderr);
        fputs(": No such file or directory\n", stderr);
        rc = 1;
        continue;
      }
    }
    if (cat_fd(fd)) rc = 1;
    if (fd != 0) close(fd);
  }
  return rc;
}
