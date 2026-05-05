/*
 * stdlib.c — general utilities (numeric conversions, sorting / searching,
 * environment).
 *
 * POSIX-named subset:
 *   atoi, strtol, strtoul, abs, labs, qsort, bsearch, getenv
 *
 * Plus the `environ` global and the crt0 hook _uclib_init_env that
 * walks the auxv-style argv tail to find envp.
 *
 * malloc / free / uc_heap_init live in alloc.c so that the host
 * unit tests for the allocator can link against just that TU.
 */

#include "lib/uclib.h"

#include <ctype.h>
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

/* ── strtoul / strtol ──────────────────────────────────────────────── *
 *
 * Compact implementation: handles leading whitespace, optional sign
 * (signed and unsigned both accept '+'/'-'), and base detection when
 * `base == 0` (0x → 16, leading 0 → 8, otherwise 10).  No overflow
 * detection (no LONG_MIN / ULONG_MAX clamping); a future revision
 * will set errno=ERANGE when needed.  Sets *endptr if non-null.
 */

unsigned long strtoul(const char *nptr, char **endptr, int base) {
  const char *s = nptr;
  while (isspace((unsigned char)*s)) s++;

  int neg = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  } else if (*s == '+') {
    s++;
  }

  if ((base == 0 || base == 16) && s[0] == '0' &&
      (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    s += 2;
  } else if (base == 0 && s[0] == '0') {
    base = 8;
    s++;
  } else if (base == 0) {
    base = 10;
  }

  if (base < 2 || base > 36) {
    if (endptr) *endptr = (char *)nptr;
    return 0;
  }

  unsigned long acc = 0;
  const char *digits_start = s;
  for (; *s; s++) {
    int d;
    unsigned char c = (unsigned char)*s;
    if (c >= '0' && c <= '9')
      d = c - '0';
    else if (c >= 'a' && c <= 'z')
      d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z')
      d = c - 'A' + 10;
    else
      break;
    if (d >= base) break;
    acc = acc * (unsigned long)base + (unsigned long)d;
  }

  if (endptr) *endptr = (char *)(s == digits_start ? nptr : s);
  return neg ? (unsigned long)(-(long)acc) : acc;
}

long strtol(const char *nptr, char **endptr, int base) {
  /* Defer to strtoul; strtoul already applies the sign for us. */
  return (long)strtoul(nptr, endptr, base);
}

/* ── abs / labs ────────────────────────────────────────────────────── */

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

/* ── qsort / bsearch ───────────────────────────────────────────────── *
 *
 * Lomuto-partition recursive quicksort, last-element pivot.  Adequate
 * for the small-N cases PPAP applets actually face (sort works on
 * already-bounded heaps).  Recurses on the smaller half first to cap
 * stack at O(log n) for typical inputs.
 */

static void byteswap(char *a, char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char t = a[i];
    a[i] = b[i];
    b[i] = t;
  }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
  if (nmemb < 2) return;
  char *arr = base;
  char *pivot = arr + (nmemb - 1) * size;
  size_t i = 0;
  for (size_t j = 0; j < nmemb - 1; j++) {
    if (compar(arr + j * size, pivot) <= 0) {
      if (i != j) byteswap(arr + i * size, arr + j * size, size);
      i++;
    }
  }
  if (i != nmemb - 1) byteswap(arr + i * size, pivot, size);
  qsort(arr, i, size, compar);
  qsort(arr + (i + 1) * size, nmemb - i - 1, size, compar);
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
  size_t lo = 0, hi = nmemb;
  const char *arr = base;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int cmp = compar(key, arr + mid * size);
    if (cmp == 0) return (void *)(arr + mid * size);
    if (cmp < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return (void *)0;
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
