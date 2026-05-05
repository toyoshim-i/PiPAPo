/*
 * util.c — PPAP user-space helpers awaiting a POSIX home.
 *
 * Each function lives under the `uc_` prefix as a TODO marker.  They
 * will move to standard headers and lose the prefix when matching POSIX
 * machinery lands:
 *
 *   uc_copy_fd            stays as PPAP extension
 *   uc_parse_u32          future strtoul()-based replacement
 */

#include "lib/uclib.h"
#include "syscall.h"

#include <stddef.h>
#include <stdint.h>

/* ── Numeric parsing ──────────────────────────────────────────────── */

int uc_parse_u32(const char *s, uint32_t *out) {
  uint32_t v = 0;
  int base = 10;
  int seen = 0;

  if (!s || !*s) return -1;

  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    s += 2;
  }

  while (*s) {
    char c = *s++;
    uint32_t d;
    if (c >= '0' && c <= '9')
      d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f')
      d = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      d = (uint32_t)(c - 'A' + 10);
    else
      return -1;
    if (d >= (uint32_t)base) return -1;
    v = v * (uint32_t)base + d;
    seen = 1;
  }

  if (!seen) return -1;
  *out = v;
  return 0;
}

/* ── File copy ───────────────────────────────────────────────────── */

long uc_copy_fd(int src_fd, int dst_fd) {
  /* 512 B matches a sector on UFS / vfat, which happens to be the
   * smallest "natural" chunk all the FS drivers are tuned for.  On
   * pcxt we also have a tight user stack, so keep it small. */
  char buf[512];
  long total = 0;
  for (;;) {
    ssize_t n = read(src_fd, buf, sizeof(buf));
    if (n == 0) return total;
    if (n < 0) return -1;
    ssize_t w = 0;
    while (w < n) {
      ssize_t m = write(dst_fd, buf + w, (size_t)(n - w));
      if (m <= 0) return -1;
      w += m;
    }
    total += (long)n;
  }
}
