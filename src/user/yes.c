/*
 * yes.c — repeatedly output a string until killed
 *
 * Usage: yes [STRING ...]
 *   With no args, prints "y\n" forever.
 *   With args, prints them space-separated, newline-terminated, forever.
 *
 * Loops on write() with a small batch buffer so the kernel sees
 * larger chunks than one write per line — keeps the output pipe
 * fed without thrashing on syscalls.
 */

#include "lib/uclib.h"

int main(int argc, char *argv[]) {
  if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
    uc_puts(
        "Usage: yes [STRING ...]\n"
        "  Repeatedly output STRING (default \"y\") until killed.\n");
    return 0;
  }

  /* Build one line into a small buffer. */
  char line[128];
  int len = 0;
  if (argc < 2) {
    line[len++] = 'y';
  } else {
    for (int i = 1; i < argc && len < (int)sizeof(line) - 2; i++) {
      const char *a = argv[i];
      if (i > 1 && len < (int)sizeof(line) - 1) line[len++] = ' ';
      while (*a && len < (int)sizeof(line) - 1) line[len++] = *a++;
    }
  }
  if (len < (int)sizeof(line)) line[len++] = '\n';

  /* One write per iteration.  Downstream pipes may have small buffers
   * (the kernel pipe is currently 512 B), so writing big batches just
   * leads to short writes anyway.  EPIPE / write-error breaks us out. */
  for (;;) {
    ssize_t off = 0;
    while (off < len) {
      ssize_t w = write(1, line + off, len - off);
      if (w <= 0) return 1;
      off += w;
    }
  }
}
