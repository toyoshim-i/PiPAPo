/*
 * printf.c — POSIX printf applet
 *
 * Usage: printf FORMAT [ARG ...]
 *
 * Format string supports backslash escapes (\n \t \r \\ \" \a \b \f \v
 * \0) and conversion specifiers: %s %d %i %u %x %X %o %c %%, with
 * optional flag (`-` left-align, `0` zero-pad), width, and precision
 * (precision is parsed and ignored — POSIX requires the syntax to be
 * accepted but PPAP printf does not implement string truncation).
 *
 * If more args are present than the format consumes, the format is
 * applied repeatedly (POSIX semantics).  If the format has no
 * conversion specifiers, no repetition occurs.
 *
 * Octal/hex backslash escapes (\NNN, \xHH) and busybox's %b extension
 * are not implemented — use busybox if you need them.
 *
 * The applet has its own format walker rather than reusing uc_vsnprintf
 * because POSIX printf takes string args and converts each one
 * lazily, while uc_vsnprintf operates on a typed va_list.
 */

#include "lib/uclib.h"

/* Parse a decimal integer, optionally signed.  Returns 0 on success
 * with *out set, -1 on parse error or trailing garbage. */
static int parse_long(const char *s, long *out) {
  if (!*s) return -1;
  int neg = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  } else if (*s == '+') {
    s++;
  }
  if (!*s) return -1;
  long v = 0;
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (*s - '0');
    s++;
  }
  if (*s) return -1;
  *out = neg ? -v : v;
  return 0;
}

static void emit_pad(int n, char c) {
  for (int i = 0; i < n; i++) uc_putc(c);
}

static void emit_str(const char *s, int width, int left_align) {
  int len = uc_strlen(s);
  if (!left_align && width > len) emit_pad(width - len, ' ');
  uc_puts(s);
  if (left_align && width > len) emit_pad(width - len, ' ');
}

/* Emit signed decimal with width/zero-pad/left-align.  Sign is placed
 * before zero padding (so -42 with %05d becomes "-0042"). */
static void emit_int(long v, int width, int zero, int left_align) {
  char digits[16];
  uint32_t u = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
  uc_snprintf(digits, (int)sizeof(digits), "%u", u);
  int dlen = uc_strlen(digits);
  int total = dlen + (v < 0 ? 1 : 0);

  if (zero && !left_align) {
    if (v < 0) uc_putc('-');
    if (width > total) emit_pad(width - total, '0');
    uc_puts(digits);
  } else if (left_align) {
    if (v < 0) uc_putc('-');
    uc_puts(digits);
    if (width > total) emit_pad(width - total, ' ');
  } else {
    if (width > total) emit_pad(width - total, ' ');
    if (v < 0) uc_putc('-');
    uc_puts(digits);
  }
}

/* Emit unsigned in base 10/16/8 with width/zero-pad/left-align. */
static void emit_uint(uint32_t v, char conv, int width, int zero,
                      int left_align) {
  char buf[16];
  if (conv == 'o') {
    char tmp[12];
    int n = 0;
    if (v == 0) {
      tmp[n++] = '0';
    } else {
      while (v) {
        tmp[n++] = (char)('0' + (v & 7));
        v >>= 3;
      }
    }
    int i = 0;
    for (; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[i] = '\0';
  } else {
    /* %u and %x supported by uc_snprintf; %X handled by upper-casing. */
    uc_snprintf(buf, (int)sizeof(buf), (conv == 'u') ? "%u" : "%x", v);
    if (conv == 'X') {
      for (int i = 0; buf[i]; i++) {
        if (buf[i] >= 'a' && buf[i] <= 'f') buf[i] = (char)(buf[i] - 32);
      }
    }
  }
  int len = uc_strlen(buf);
  char pad = (zero && !left_align) ? '0' : ' ';
  if (!left_align && width > len) emit_pad(width - len, pad);
  uc_puts(buf);
  if (left_align && width > len) emit_pad(width - len, ' ');
}

/* Process a backslash escape at p[1].  Returns the number of bytes
 * after p[0]='\\' that were consumed (1 for known escapes), or 0 if
 * the next byte is unknown — caller should output '\\' literally. */
static int do_backslash(char c) {
  switch (c) {
    case 'n': uc_putc('\n'); return 1;
    case 't': uc_putc('\t'); return 1;
    case 'r': uc_putc('\r'); return 1;
    case '\\': uc_putc('\\'); return 1;
    case 'a': uc_putc('\a'); return 1;
    case 'b': uc_putc('\b'); return 1;
    case 'f': uc_putc('\f'); return 1;
    case 'v': uc_putc('\v'); return 1;
    case '0': uc_putc('\0'); return 1;
    case '"': uc_putc('"'); return 1;
    default:  return 0;
  }
}

/* Run one pass over `fmt`, drawing args from args[0..args_left).
 * Returns the number of args consumed.  Sets *had_spec if the format
 * contained at least one conversion specifier (not counting %%). */
static int run_format(const char *fmt, char **args, int args_left,
                      int *had_spec) {
  int used = 0;
  *had_spec = 0;

  for (const char *p = fmt; *p;) {
    if (*p == '\\' && p[1]) {
      int n = do_backslash(p[1]);
      if (n == 0) {
        uc_putc('\\');
        p++;
      } else {
        p += 1 + n;
      }
      continue;
    }
    if (*p != '%') {
      uc_putc(*p++);
      continue;
    }
    p++; /* skip '%' */

    int left_align = 0, zero = 0;
    while (*p == '-' || *p == '0' || *p == '+' || *p == ' ' || *p == '#') {
      if (*p == '-') left_align = 1;
      else if (*p == '0') zero = 1;
      /* +, space, # accepted but ignored */
      p++;
    }
    int width = 0;
    while (*p >= '0' && *p <= '9') {
      width = width * 10 + (*p - '0');
      p++;
    }
    if (*p == '.') {
      p++;
      while (*p >= '0' && *p <= '9') p++;
    }
    char conv = *p;
    if (conv == '\0') break;
    p++;

    if (conv == '%') {
      uc_putc('%');
      continue;
    }
    *had_spec = 1;

    const char *arg = (used < args_left) ? args[used++] : "";

    switch (conv) {
      case 's':
        emit_str(arg, width, left_align);
        break;
      case 'c':
        if (*arg) uc_putc(*arg);
        break;
      case 'd':
      case 'i': {
        long v = 0;
        if (*arg) parse_long(arg, &v);
        emit_int(v, width, zero, left_align);
        break;
      }
      case 'u':
      case 'x':
      case 'X':
      case 'o': {
        long v = 0;
        if (*arg) parse_long(arg, &v);
        emit_uint((uint32_t)v, conv, width, zero, left_align);
        break;
      }
      default:
        uc_putc('%');
        uc_putc(conv);
        break;
    }
  }
  return used;
}

int main(int argc, char *argv[]) {
  if (argc < 2 || uc_strcmp(argv[1], "--help") == 0) {
    uc_eputs(
        "Usage: printf FORMAT [ARG ...]\n"
        "  Specifiers: %s %d %i %u %x %X %o %c %%\n"
        "  Flags: - 0    Width: integer    Precision: parsed, ignored\n"
        "  Escapes: \\n \\t \\r \\\\ \\\" \\a \\b \\f \\v \\0\n");
    return (argc < 2) ? 1 : 0;
  }

  const char *fmt = argv[1];
  char **args = argv + 2;
  int args_left = argc - 2;

  int had_spec = 0;
  int used = run_format(fmt, args, args_left, &had_spec);
  if (!had_spec) return 0;

  args += used;
  args_left -= used;
  while (args_left > 0) {
    int had_spec2 = 0;
    int u2 = run_format(fmt, args, args_left, &had_spec2);
    if (u2 == 0) break; /* safety: no progress, avoid infinite loop */
    args += u2;
    args_left -= u2;
  }
  return 0;
}
