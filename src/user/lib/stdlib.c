/*
 * stdlib.c — general utilities (numeric conversions, environment).
 *
 * POSIX-named subset:
 *   atoi, getenv
 *
 * Plus the `environ` global and the crt0 hook _uclib_init_env that
 * walks the auxv-style argv tail to find envp.
 *
 * malloc / free / uc_heap_init live in alloc.c so that the host
 * unit tests for the allocator can link against just that TU.
 */

#include "lib/uclib.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int atoi(const char *s) {
  int neg = 0;
  int v = 0;

  while (*s == ' ' || *s == '\t') s++;
  if (*s == '-') {
    neg = 1;
    s++;
  } else if (*s == '+') {
    s++;
  }
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return neg ? -v : v;
}

/* The kernel lays out argc, argv[], NULL, envp[], NULL on the initial
 * user stack.  _uclib_init_env is called from crt0 with argc/argv in
 * the C-ABI argument registers; it stores &argv[argc+1] (the first
 * envp entry) into the global `environ`.  getenv walks that array. */

char **environ = 0;

void _uclib_init_env(int argc, char **argv) {
  if (argc < 0 || !argv) {
    environ = 0;
    return;
  }
  environ = argv + argc + 1;
}

char *getenv(const char *name) {
  if (!environ || !name) return 0;
  size_t nlen = strlen(name);
  for (char **p = environ; *p; p++) {
    char *s = *p;
    if (strncmp(s, name, nlen) == 0 && s[nlen] == '=') return s + nlen + 1;
  }
  return 0;
}
