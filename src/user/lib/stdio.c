/*
 * stdio.c — formatted-output engine.
 *
 *   snprintf / vsnprintf  buffer-only formatting (no FILE involvement)
 *   printf / vprintf      → vfprintf(stdout, …)
 *   fprintf / vfprintf    formatted FILE-stream output
 *   putchar               → fputc(c, stdout)
 *
 * Supported conversions: %s %d %u %x %c %% with optional zero-pad and
 * width.  Not yet: %f %e %g %p %o, length modifiers (l, h), precision,
 * left-justify, sign flags.
 *
 * The FILE-stream surface (fopen, fread, fwrite, fputs, …) lives in
 * file.c.
 */

#include "lib/uclib.h"
#include "syscall.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static const char hex_digits[] = "0123456789abcdef";

/* Emit one char into buf if space remains. */
static void sn_emit(char *buf, size_t size, size_t *pos, char c) {
  if (*pos < size - 1) buf[*pos] = c;
  (*pos)++;
}

/* Emit a NUL-terminated string. */
static void sn_str(char *buf, size_t size, size_t *pos, const char *s) {
  while (*s) sn_emit(buf, size, pos, *s++);
}

/* Emit an unsigned decimal with optional width/zero-pad. */
static void sn_uint(char *buf, size_t size, size_t *pos, uint32_t v, int width,
                    int zero) {
  char tmp[10];
  int n = 0;

  if (v == 0) {
    tmp[n++] = '0';
  } else {
    while (v) {
      tmp[n++] = (char)('0' + v % 10);
      v /= 10;
    }
  }
  char pad = zero ? '0' : ' ';
  for (int i = n; i < width; i++) sn_emit(buf, size, pos, pad);
  for (int i = n - 1; i >= 0; i--) sn_emit(buf, size, pos, tmp[i]);
}

/* Emit a hex value with optional width/zero-pad. */
static void sn_hex(char *buf, size_t size, size_t *pos, uint32_t v, int width,
                   int zero) {
  char tmp[8];
  int n = 0;

  if (v == 0) {
    tmp[n++] = '0';
  } else {
    while (v) {
      tmp[n++] = hex_digits[v & 0xf];
      v >>= 4;
    }
  }
  char pad = zero ? '0' : ' ';
  for (int i = n; i < width; i++) sn_emit(buf, size, pos, pad);
  for (int i = n - 1; i >= 0; i--) sn_emit(buf, size, pos, tmp[i]);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
  size_t pos = 0;

  if (size == 0) return 0;

  while (*fmt) {
    if (*fmt != '%') {
      sn_emit(buf, size, &pos, *fmt++);
      continue;
    }
    fmt++; /* skip '%' */

    /* Parse optional zero-pad and width. */
    int zero = 0;
    int width = 0;
    if (*fmt == '0') {
      zero = 1;
      fmt++;
    }
    while (*fmt >= '0' && *fmt <= '9') {
      width = width * 10 + (*fmt - '0');
      fmt++;
    }

    switch (*fmt) {
      case 's':
        sn_str(buf, size, &pos, va_arg(ap, const char *));
        break;
      case 'd': {
        int32_t v = va_arg(ap, int32_t);
        if (v < 0) {
          sn_emit(buf, size, &pos, '-');
          sn_uint(buf, size, &pos, (uint32_t)(-v), width, zero);
        } else {
          sn_uint(buf, size, &pos, (uint32_t)v, width, zero);
        }
        break;
      }
      case 'u':
        sn_uint(buf, size, &pos, va_arg(ap, uint32_t), width, zero);
        break;
      case 'x':
        sn_hex(buf, size, &pos, va_arg(ap, uint32_t), width, zero);
        break;
      case 'c':
        sn_emit(buf, size, &pos, (char)va_arg(ap, int));
        break;
      case '%':
        sn_emit(buf, size, &pos, '%');
        break;
      case '\0':
        goto done;
      default:
        sn_emit(buf, size, &pos, '%');
        sn_emit(buf, size, &pos, *fmt);
        break;
    }
    fmt++;
  }
done:
  /* NUL-terminate; pos may exceed size-1 (reports would-have-written). */
  if (pos < size)
    buf[pos] = '\0';
  else
    buf[size - 1] = '\0';
  return (int)pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return n;
}

/* ── FILE-routed formatted output ─────────────────────────────────── *
 *
 * Format into a stack scratch buffer, then push through fwrite so any
 * caller-installed buffering on the stream is honoured.  Output that
 * exceeds the scratch is silently truncated; callers that need more
 * should snprintf into their own buffer + fwrite.
 */

int vfprintf(FILE *fp, const char *fmt, va_list ap) {
  char scratch[256];
  int n = vsnprintf(scratch, sizeof(scratch), fmt, ap);
  if (n > (int)sizeof(scratch) - 1) n = (int)sizeof(scratch) - 1;
  if (n <= 0) return 0;
  if (fwrite(scratch, 1, (size_t)n, fp) != (size_t)n) return -1;
  return n;
}

int fprintf(FILE *fp, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vfprintf(fp, fmt, ap);
  va_end(ap);
  return n;
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vfprintf(stdout, fmt, ap);
  va_end(ap);
  return n;
}

/* putchar — equivalent to fputc(c, stdout) per POSIX.  fputc handles
 * any buffering the caller may have installed on stdout. */
int putchar(int c) { return fputc(c, stdout); }
