/*
 * <time.h> — POSIX time / calendar.
 *
 * struct timespec is forwarded from src/common/time.h.  PPAP has no
 * timezone support: localtime() and gmtime() produce the same result
 * (UTC).  No leap-second handling.
 *
 * Not provided: difftime (no float), clock(), wide-char strftime.
 */

#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>

#include "common/time.h" /* struct timespec */

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

typedef long time_t;
typedef long clock_t;

struct tm {
  int tm_sec;
  int tm_min;
  int tm_hour;
  int tm_mday;
  int tm_mon;   /* 0-11 */
  int tm_year;  /* years since 1900 */
  int tm_wday;  /* 0-6, Sunday = 0 */
  int tm_yday;  /* 0-365 */
  int tm_isdst; /* always 0 — PPAP has no DST */
};

time_t time(time_t *t);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime_r(const time_t *t, struct tm *out);
time_t mktime(struct tm *t);
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *t);

/* Convenience re-declarations matching the syscall signatures. */
int clock_gettime(int clk_id, void *tp);
int nanosleep(const void *req, void *rem);

#endif /* _TIME_H */
