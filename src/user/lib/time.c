/*
 * time.c — calendar time and broken-down time.
 *
 * UTC-only.  No timezone support: localtime() == gmtime().  No leap
 * seconds.  Year range: 1970 .. 2106 (32-bit time_t).  strftime
 * implements the subset useful for ls / date / log timestamps:
 *   %Y %y %m %d %H %M %S %a %A %b %B %j %w %% %n %t %F %T %s
 */

#include "lib/uclib.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

static const uint8_t mdays[12] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};

static int is_leap(int year) {
  return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

time_t time(time_t *out) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) < 0) return (time_t)-1;
  if (out) *out = (time_t)ts.tv_sec;
  return (time_t)ts.tv_sec;
}

struct tm *gmtime_r(const time_t *t, struct tm *out) {
  if (!t || !out) return (void *)0;
  long epoch = (long)*t;
  out->tm_sec = (int)(epoch % 60);
  epoch /= 60;
  out->tm_min = (int)(epoch % 60);
  epoch /= 60;
  out->tm_hour = (int)(epoch % 24);
  long days = epoch / 24;

  /* Jan 1, 1970 was a Thursday (4 in 0=Sunday convention). */
  out->tm_wday = (int)(((days % 7) + 4) % 7);
  if (out->tm_wday < 0) out->tm_wday += 7;

  int year = 1970;
  for (;;) {
    long ydays = is_leap(year) ? 366 : 365;
    if (days < ydays) break;
    days -= ydays;
    year++;
  }
  out->tm_year = year - 1900;
  out->tm_yday = (int)days;
  out->tm_isdst = 0;

  int leap = is_leap(year);
  for (int m = 0; m < 12; m++) {
    int md = mdays[m] + ((m == 1 && leap) ? 1 : 0);
    if (days < md) {
      out->tm_mon = m;
      out->tm_mday = (int)days + 1;
      return out;
    }
    days -= md;
  }
  /* Unreachable. */
  out->tm_mon = 11;
  out->tm_mday = 31;
  return out;
}

struct tm *localtime_r(const time_t *t, struct tm *out) {
  return gmtime_r(t, out);
}

static struct tm shared_tm;

struct tm *gmtime(const time_t *t) { return gmtime_r(t, &shared_tm); }
struct tm *localtime(const time_t *t) { return localtime_r(t, &shared_tm); }

time_t mktime(struct tm *t) {
  long year = t->tm_year + 1900;
  long days = 0;
  for (long y = 1970; y < year; y++) days += is_leap((int)y) ? 366 : 365;
  long yday = 0;
  int leap = is_leap((int)year);
  for (int m = 0; m < t->tm_mon; m++) {
    yday += mdays[m] + ((m == 1 && leap) ? 1 : 0);
  }
  yday += t->tm_mday - 1;
  long total_days = days + yday;
  long secs = total_days * 86400L + (long)t->tm_hour * 3600 +
              (long)t->tm_min * 60 + (long)t->tm_sec;
  t->tm_wday = (int)(((total_days % 7) + 4) % 7);
  if (t->tm_wday < 0) t->tm_wday += 7;
  t->tm_yday = (int)yday;
  t->tm_isdst = 0;
  return (time_t)secs;
}

/* ── strftime ──────────────────────────────────────────────────────── */

static const char *day_short[] = {"Sun", "Mon", "Tue", "Wed",
                                  "Thu", "Fri", "Sat"};
static const char *day_full[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                 "Thursday", "Friday", "Saturday"};
static const char *mon_short[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *mon_full[] = {"January", "February", "March",     "April",
                                 "May",     "June",     "July",      "August",
                                 "September", "October", "November", "December"};

static size_t emit_str(char *s, size_t max, size_t pos, const char *src) {
  while (*src) {
    if (pos < max - 1) s[pos] = *src;
    pos++;
    src++;
  }
  return pos;
}

static size_t emit_dec(char *s, size_t max, size_t pos, long v, int width) {
  char tmp[12];
  int n = 0;
  int neg = 0;
  if (v < 0) {
    neg = 1;
    v = -v;
  }
  if (v == 0) tmp[n++] = '0';
  while (v) {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  if (neg) tmp[n++] = '-';
  for (int i = n; i < width; i++) {
    if (pos < max - 1) s[pos] = '0';
    pos++;
  }
  for (int i = n - 1; i >= 0; i--) {
    if (pos < max - 1) s[pos] = tmp[i];
    pos++;
  }
  return pos;
}

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *t) {
  if (max == 0) return 0;
  size_t pos = 0;
  while (*fmt) {
    if (*fmt != '%') {
      if (pos < max - 1) s[pos] = *fmt;
      pos++;
      fmt++;
      continue;
    }
    fmt++;
    int wday = t->tm_wday;
    if (wday < 0 || wday > 6) wday = 0;
    int mon = t->tm_mon;
    if (mon < 0 || mon > 11) mon = 0;

    switch (*fmt) {
      case 'Y': pos = emit_dec(s, max, pos, t->tm_year + 1900, 4); break;
      case 'y': pos = emit_dec(s, max, pos, (t->tm_year + 1900) % 100, 2); break;
      case 'm': pos = emit_dec(s, max, pos, t->tm_mon + 1, 2); break;
      case 'd': pos = emit_dec(s, max, pos, t->tm_mday, 2); break;
      case 'H': pos = emit_dec(s, max, pos, t->tm_hour, 2); break;
      case 'M': pos = emit_dec(s, max, pos, t->tm_min, 2); break;
      case 'S': pos = emit_dec(s, max, pos, t->tm_sec, 2); break;
      case 'a': pos = emit_str(s, max, pos, day_short[wday]); break;
      case 'A': pos = emit_str(s, max, pos, day_full[wday]); break;
      case 'b':
      case 'h': pos = emit_str(s, max, pos, mon_short[mon]); break;
      case 'B': pos = emit_str(s, max, pos, mon_full[mon]); break;
      case 'j': pos = emit_dec(s, max, pos, t->tm_yday + 1, 3); break;
      case 'w': pos = emit_dec(s, max, pos, wday, 0); break;
      case '%':
        if (pos < max - 1) s[pos] = '%';
        pos++;
        break;
      case 'n':
        if (pos < max - 1) s[pos] = '\n';
        pos++;
        break;
      case 't':
        if (pos < max - 1) s[pos] = '\t';
        pos++;
        break;
      case 'F':
        pos = emit_dec(s, max, pos, t->tm_year + 1900, 4);
        if (pos < max - 1) s[pos] = '-';
        pos++;
        pos = emit_dec(s, max, pos, t->tm_mon + 1, 2);
        if (pos < max - 1) s[pos] = '-';
        pos++;
        pos = emit_dec(s, max, pos, t->tm_mday, 2);
        break;
      case 'T':
        pos = emit_dec(s, max, pos, t->tm_hour, 2);
        if (pos < max - 1) s[pos] = ':';
        pos++;
        pos = emit_dec(s, max, pos, t->tm_min, 2);
        if (pos < max - 1) s[pos] = ':';
        pos++;
        pos = emit_dec(s, max, pos, t->tm_sec, 2);
        break;
      case 's': {
        struct tm copy = *t;
        time_t epoch = mktime(&copy);
        pos = emit_dec(s, max, pos, (long)epoch, 0);
        break;
      }
      case '\0': goto done;
      default:
        if (pos < max - 1) s[pos] = '%';
        pos++;
        if (pos < max - 1) s[pos] = *fmt;
        pos++;
        break;
    }
    fmt++;
  }
done:
  if (pos < max) s[pos] = '\0';
  else s[max - 1] = '\0';
  return pos < max ? pos : 0; /* POSIX: return 0 if truncated */
}
