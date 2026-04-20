/*
 * uclib.c — micro C library for PPAP user space
 *
 * Build with -ffunction-sections -fdata-sections so each function
 * lands in its own ELF section.  Link with --gc-sections to strip
 * anything the app doesn't reference.
 */

#include "lib/uclib.h"

#include <stdarg.h>

/* ── formatted output ──────────────────────────────────────────────── */

void uc_puts(const char *s) {
  int n = 0;
  while (s[n]) n++;
  write(1, s, (size_t)n);
}

void uc_eputs(const char *s) {
  int n = 0;
  while (s[n]) n++;
  write(2, s, (size_t)n);
}

void uc_putc(char c) { write(1, &c, 1); }

void uc_putu(uint32_t v) {
  static const uint32_t pw[] = {1000000000u, 100000000u, 10000000u, 1000000u,
                                100000u,     10000u,     1000u,     100u,
                                10u,         1u};
  int started = 0;

  if (v == 0) {
    uc_putc('0');
    return;
  }
  for (int i = 0; i < 10; i++) {
    uint32_t d = 0;
    while (v >= pw[i]) {
      v -= pw[i];
      d++;
    }
    if (d || started) {
      uc_putc((char)('0' + d));
      started = 1;
    }
  }
}

void uc_puti(int32_t v) {
  if (v < 0) {
    uc_putc('-');
    uc_putu((uint32_t)(-v));
    return;
  }
  uc_putu((uint32_t)v);
}

static const char hex_digits[] = "0123456789abcdef";

void uc_putx32(uint32_t v) {
  uc_puts("0x");
  for (int s = 28; s >= 0; s -= 4) uc_putc(hex_digits[(v >> s) & 0xf]);
}

void uc_putx16(uint32_t v) {
  uc_puts("0x");
  for (int s = 12; s >= 0; s -= 4) uc_putc(hex_digits[(v >> s) & 0xf]);
}

void uc_putx8(uint32_t v) {
  uc_puts("0x");
  uc_putc(hex_digits[(v >> 4) & 0xf]);
  uc_putc(hex_digits[v & 0xf]);
}

/* ── snprintf engine ───────────────────────────────────────────────── */

/* Emit one char into buf if space remains. */
static void sn_emit(char *buf, int size, int *pos, char c) {
  if (*pos < size - 1) buf[*pos] = c;
  (*pos)++;
}

/* Emit a NUL-terminated string. */
static void sn_str(char *buf, int size, int *pos, const char *s) {
  while (*s) sn_emit(buf, size, pos, *s++);
}

/* Emit an unsigned decimal with optional width/zero-pad. */
static void sn_uint(char *buf, int size, int *pos, uint32_t v, int width,
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
static void sn_hex(char *buf, int size, int *pos, uint32_t v, int width,
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

int uc_snprintf(char *buf, int size, const char *fmt, ...) {
  va_list ap;
  int pos = 0;

  if (size <= 0) return 0;

  va_start(ap, fmt);
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
  va_end(ap);

  /* NUL-terminate; pos may exceed size-1 (reports would-have-written). */
  if (pos < size)
    buf[pos] = '\0';
  else
    buf[size - 1] = '\0';
  return pos;
}

/* ── string operations ─────────────────────────────────────────────── */

int uc_strlen(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

int uc_strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

int uc_strncmp(const char *a, const char *b, int n) {
  for (int i = 0; i < n; i++) {
    if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    if (!a[i]) return 0;
  }
  return 0;
}

char *uc_strcpy(char *dst, const char *src) {
  char *d = dst;
  while ((*d++ = *src++))
    ;
  return dst;
}

char *uc_strncpy(char *dst, const char *src, int n) {
  int i;
  for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
  for (; i < n; i++) dst[i] = '\0';
  return dst;
}

char *uc_strchr(const char *s, int c) {
  for (; *s; s++)
    if (*s == (char)c) return (char *)s;
  return (c == 0) ? (char *)s : (void *)0;
}

char *uc_strrchr(const char *s, int c) {
  const char *last = (void *)0;
  for (; *s; s++)
    if (*s == (char)c) last = s;
  if (c == 0) return (char *)s;
  return (char *)last;
}

/* ── memory operations ─────────────────────────────────────────────── */

void *uc_memcpy(void *dst, const void *src, int n) {
  char *d = dst;
  const char *s = src;
  for (int i = 0; i < n; i++) d[i] = s[i];
  return dst;
}

/* Freestanding user builds may emit memcpy for struct copies.
 * Keep this symbol in shared uclib so all targets have one consistent provider.
 * ia16 runtime.c intentionally does not define memcpy to avoid duplicate symbols. */
void *memcpy(void *dst, const void *src, unsigned int n) {
  return uc_memcpy(dst, src, (int)n);
}

void *uc_memset(void *dst, int c, int n) {
  char *d = dst;
  for (int i = 0; i < n; i++) d[i] = (char)c;
  return dst;
}

int uc_memcmp(const void *a, const void *b, int n) {
  const unsigned char *p = a, *q = b;
  for (int i = 0; i < n; i++)
    if (p[i] != q[i]) return p[i] - q[i];
  return 0;
}

/* ── numeric parsing ───────────────────────────────────────────────── */

int uc_atoi(const char *s) {
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

/* ── path helpers ──────────────────────────────────────────────────── */

const char *uc_basename(const char *path) {
  const char *last = path;
  for (const char *p = path; *p; p++)
    if (*p == '/') last = p + 1;
  return last;
}

/* ── environment ───────────────────────────────────────────────────── *
 *
 * The kernel lays out argc, argv[], NULL, envp[], NULL on the initial
 * user stack (standard POSIX layout, built by the ELF / ELF16 loaders).
 * _uclib_init_env is called from crt0 with argc/argv in the C-ABI
 * argument registers; it stores &argv[argc+1] (the first envp entry)
 * into the global `environ`.  uc_getenv walks that array. */

char **environ = 0;

void _uclib_init_env(int argc, char **argv) {
  if (argc < 0 || !argv) {
    environ = 0;
    return;
  }
  environ = argv + argc + 1;
}

const char *uc_getenv(const char *name) {
  if (!environ || !name) return 0;
  int nlen = uc_strlen(name);
  for (char **p = environ; *p; p++) {
    const char *s = *p;
    if (uc_strncmp(s, name, nlen) == 0 && s[nlen] == '=') return s + nlen + 1;
  }
  return 0;
}
