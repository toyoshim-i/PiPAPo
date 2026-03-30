/*
 * cat.c — concatenate and print files
 *
 * Usage: cat [file ...]
 * With no arguments, copies stdin to stdout.
 */

#include "lib/uclib.h"

static int cat_fd(int fd) {
  char buf[256];
  ssize_t n;

  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    ssize_t off = 0;
    while (off < n) {
      ssize_t w = write(1, buf + off, (size_t)(n - off));
      if (w <= 0) return 1;
      off += w;
    }
  }
  return (n < 0) ? 1 : 0;
}

int main(int argc, char *argv[]) {
  if (argc <= 1) return cat_fd(0);

  int rc = 0;
  for (int i = 1; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY, 0);
    if (fd < 0) {
      uc_eputs("cat: ");
      uc_eputs(argv[i]);
      uc_eputs(": No such file or directory\n");
      rc = 1;
      continue;
    }
    if (cat_fd(fd)) rc = 1;
    close(fd);
  }
  return rc;
}
