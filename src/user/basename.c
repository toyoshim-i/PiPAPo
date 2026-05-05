/*
 * basename.c — POSIX basename
 *
 * Usage: basename PATH [SUFFIX]
 *   Strip directory components and trailing slashes from PATH.
 *   If SUFFIX is given and matches the trailing characters of the
 *   resulting basename (and would not consume the entire string),
 *   strip it as well.
 *
 * POSIX edge cases:
 *   basename ""        -> "."
 *   basename "/"       -> "/"
 *   basename "/a/"     -> "a"
 *   basename "/a/b"    -> "b"
 *   basename "foo.txt" .txt -> "foo"
 *   basename "foo" foo -> "foo"   (suffix == whole base, no strip)
 *
 * uc_basename in uclib intentionally implements only the simple
 * "scan for last '/'" form for callers that pass clean paths; this
 * applet has its own POSIX-correct walker.
 */

#include "lib/uclib.h"

int main(int argc, char *argv[]) {
  if (argc < 2 || argc > 3 || strcmp(argv[1], "--help") == 0) {
    fputs("Usage: basename PATH [SUFFIX]\n", stderr);
    return (argc < 2 || argc > 3) ? 1 : 0;
  }

  const char *path = argv[1];
  const char *suffix = (argc == 3) ? argv[2] : 0;

  if (!path[0]) {
    fputs(".", stdout);
    putchar('\n');
    return 0;
  }

  int len = strlen(path);
  while (len > 1 && path[len - 1] == '/') len--;

  /* String of nothing but slashes collapsed to the leading '/'. */
  if (len == 1 && path[0] == '/') {
    fputs("/", stdout);
    putchar('\n');
    return 0;
  }

  int start = 0;
  for (int i = len - 1; i >= 0; i--) {
    if (path[i] == '/') {
      start = i + 1;
      break;
    }
  }
  int base_len = len - start;

  if (suffix && suffix[0]) {
    int slen = strlen(suffix);
    if (base_len > slen) {
      int match = 1;
      for (int i = 0; i < slen; i++) {
        if (path[start + base_len - slen + i] != suffix[i]) {
          match = 0;
          break;
        }
      }
      if (match) base_len -= slen;
    }
  }

  for (int i = 0; i < base_len; i++) putchar(path[start + i]);
  putchar('\n');
  return 0;
}
