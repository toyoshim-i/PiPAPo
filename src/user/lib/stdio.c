/*
 * stdio.c — formatted I/O.
 *
 * POSIX-named subset:
 *   putchar, snprintf, vsnprintf, printf
 *
 * PPAP-specific stopgaps that survive until FILE streams arrive
 * (kept under the `uc_` prefix as a TODO marker):
 *   uc_puts            stdout, no auto-newline
 *   uc_eputs / uc_eprintf  stderr-targeted
 *   uc_putu / uc_puti / uc_putx{8,16,32}  unbuffered numeric output
 */

#include "lib/uclib.h"
#include "syscall.h"

#include <stdarg.h>
#include <stdio.h>

/* ── Unbuffered numeric output (PPAP extensions) ──────────────────────── */

void uc_puts(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  write(1, s, n);
}

void uc_eputs(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  write(2, s, n);
}

int putchar(int c) {
  unsigned char ch = (unsigned char)c;
  write(1, &ch, 1);
  return ch;
}

void uc_putu(uint32_t v) {
  static const uint32_t pw[] = {1000000000u, 100000000u, 10000000u, 1000000u,
                                100000u,     10000u,     1000u,     100u,
                                10u,         1u};
  int started = 0;

  if (v == 0) {
    putchar('0');
    return;
  }
  for (int i = 0; i < 10; i++) {
    uint32_t d = 0;
    while (v >= pw[i]) {
      v -= pw[i];
      d++;
    }
    if (d || started) {
      putchar((int)('0' + d));
      started = 1;
    }
  }
}

void uc_puti(int32_t v) {
  if (v < 0) {
    putchar('-');
    uc_putu((uint32_t)(-v));
    return;
  }
  uc_putu((uint32_t)v);
}

static const char hex_digits[] = "0123456789abcdef";

void uc_putx32(uint32_t v) {
  uc_puts("0x");
  for (int s = 28; s >= 0; s -= 4) putchar(hex_digits[(v >> s) & 0xf]);
}

void uc_putx16(uint32_t v) {
  uc_puts("0x");
  for (int s = 12; s >= 0; s -= 4) putchar(hex_digits[(v >> s) & 0xf]);
}

void uc_putx8(uint32_t v) {
  uc_puts("0x");
  putchar(hex_digits[(v >> 4) & 0xf]);
  putchar(hex_digits[v & 0xf]);
}

/* ── snprintf engine ───────────────────────────────────────────────── */

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

/* Internal helper for printf / uc_eprintf — formats into a 256-byte
 * stack buffer and writes the result to `fd`.  Output exceeding 255
 * bytes is silently truncated; callers that need more should
 * snprintf into their own buffer and write directly. */
static void fdprintf(int fd, const char *fmt, va_list ap) {
  char buf[256];
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
  if (n > 0) write(fd, buf, (size_t)n);
}

int printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fdprintf(1, fmt, ap);
  va_end(ap);
  return 0; /* TODO M4: return actual byte count */
}

void uc_eprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fdprintf(2, fmt, ap);
  va_end(ap);
}
