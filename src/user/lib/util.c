/*
 * util.c — PPAP user-space helpers awaiting a POSIX home.
 *
 * Each function lives under the `uc_` prefix as a TODO marker.  They
 * will move to standard headers and lose the prefix when matching POSIX
 * machinery lands:
 *
 *   uc_basename           future <libgen.h>
 *   uc_copy_fd            stays as PPAP extension
 *   uc_parse_u32          future strtoul()-based replacement
 *   uc_gmtime, struct uc_tm, uc_format_ymdhm
 *                         future <time.h>: gmtime / struct tm / strftime
 */

#include "lib/uclib.h"
#include "syscall.h"

#include <stddef.h>
#include <stdint.h>

/* ── Path ─────────────────────────────────────────────────────────── */

const char *uc_basename(const char *path) {
  const char *last = path;
  for (const char *p = path; *p; p++)
    if (*p == '/') last = p + 1;
  return last;
}

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

/* ── Calendar ────────────────────────────────────────────────────── */

void uc_gmtime(uint32_t epoch, struct uc_tm *out) {
  out->sec = (int)(epoch % 60u);
  epoch /= 60u;
  out->min = (int)(epoch % 60u);
  epoch /= 60u;
  out->hour = (int)(epoch % 24u);
  uint32_t days = epoch / 24u;

  int year = 1970;
  for (;;) {
    int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    uint32_t ydays = leap ? 366u : 365u;
    if (days < ydays) break;
    days -= ydays;
    year++;
  }
  out->year = year;

  static const uint8_t mdays[12] = {31, 28, 31, 30, 31, 30,
                                    31, 31, 30, 31, 30, 31};
  int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
  for (int m = 0; m < 12; m++) {
    uint32_t md = mdays[m] + ((m == 1 && leap) ? 1u : 0u);
    if (days < md) {
      out->mon = m + 1;
      out->mday = (int)days + 1;
      return;
    }
    days -= md;
  }
  /* Unreachable: days always lands in one of the 12 months above. */
  out->mon = 12;
  out->mday = 31;
}

void uc_format_ymdhm(char buf[17], uint32_t epoch) {
  struct uc_tm t;
  uc_gmtime(epoch, &t);
  buf[0] = (char)('0' + (t.year / 1000) % 10);
  buf[1] = (char)('0' + (t.year / 100) % 10);
  buf[2] = (char)('0' + (t.year / 10) % 10);
  buf[3] = (char)('0' + t.year % 10);
  buf[4] = '-';
  buf[5] = (char)('0' + (t.mon / 10) % 10);
  buf[6] = (char)('0' + t.mon % 10);
  buf[7] = '-';
  buf[8] = (char)('0' + (t.mday / 10) % 10);
  buf[9] = (char)('0' + t.mday % 10);
  buf[10] = ' ';
  buf[11] = (char)('0' + (t.hour / 10) % 10);
  buf[12] = (char)('0' + t.hour % 10);
  buf[13] = ':';
  buf[14] = (char)('0' + (t.min / 10) % 10);
  buf[15] = (char)('0' + t.min % 10);
  buf[16] = '\0';
}
