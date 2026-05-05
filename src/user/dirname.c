/*
 * dirname.c — POSIX dirname
 *
 * Usage: dirname PATH
 *   Print the directory portion of PATH (everything before the
 *   final '/'-separated component, with trailing slashes removed).
 *
 * POSIX edge cases:
 *   dirname ""        -> "."
 *   dirname "/"       -> "/"
 *   dirname "/a"      -> "/"
 *   dirname "/a/"     -> "/"
 *   dirname "a"       -> "."
 *   dirname "a/b"     -> "a"
 *   dirname "/a/b/"   -> "/a"
 */

#include "lib/uclib.h"

int main(int argc, char *argv[]) {
  if (argc != 2 || strcmp(argv[1], "--help") == 0) {
    uc_eputs("Usage: dirname PATH\n");
    return (argc != 2) ? 1 : 0;
  }

  const char *path = argv[1];

  if (!path[0]) {
    uc_puts(".");
    putchar('\n');
    return 0;
  }

  int len = strlen(path);
  while (len > 1 && path[len - 1] == '/') len--;

  int last_slash = -1;
  for (int i = len - 1; i >= 0; i--) {
    if (path[i] == '/') {
      last_slash = i;
      break;
    }
  }

  /* Bare basename (no slash at all) -> ".". */
  if (last_slash < 0) {
    uc_puts(".");
    putchar('\n');
    return 0;
  }

  /* Trim trailing slashes from the dir portion (handles "//foo"). */
  int dir_len = last_slash;
  while (dir_len > 1 && path[dir_len - 1] == '/') dir_len--;

  /* Only the leading '/' survived -> root. */
  if (dir_len == 0) {
    uc_puts("/");
    putchar('\n');
    return 0;
  }

  for (int i = 0; i < dir_len; i++) putchar(path[i]);
  putchar('\n');
  return 0;
}
