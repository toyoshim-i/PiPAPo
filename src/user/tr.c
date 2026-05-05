/*
 * tr.c — translate or delete characters
 *
 * Usage:
 *   tr SET1 SET2     translate each char in SET1 to the positional
 *                    counterpart in SET2.  If SET2 is shorter, the
 *                    last char of SET2 pads the remainder (POSIX).
 *   tr -d SET1        delete characters in SET1.
 *
 * Reads stdin, writes stdout.  No file argument support — pipe in.
 *
 * SET grammar:
 *   abc          literal characters
 *   a-z          range (lo <= hi)
 *   \n \t \r \\  backslash escapes
 *   [:NAME:]     POSIX class — alpha, digit, upper, lower, space, alnum
 *   -            literal dash if at start or end of SET
 *
 * Not implemented: -c (complement), -s (squeeze), octal/hex escapes,
 * file arguments.  Use busybox if you need them.
 */

#include "lib/uclib.h"

/* 256-bit membership bitmap, packed into 32 bytes. */
typedef unsigned char bitmap_t[32];

static inline void bm_set(bitmap_t b, unsigned char c) {
  b[c >> 3] |= 1u << (c & 7);
}
static inline int bm_get(const bitmap_t b, unsigned char c) {
  return (b[c >> 3] >> (c & 7)) & 1;
}

/* Append the chars produced by a "[:NAME:]" class.  Returns the
 * number of chars appended, or -1 if NAME is unknown. */
static int append_class(const char *name, char *out, bitmap_t bm) {
  int added = 0;
  if (strcmp(name, "alpha") == 0) {
    for (int c = 'A'; c <= 'Z'; c++) { out[added++] = c; bm_set(bm, c); }
    for (int c = 'a'; c <= 'z'; c++) { out[added++] = c; bm_set(bm, c); }
  } else if (strcmp(name, "digit") == 0) {
    for (int c = '0'; c <= '9'; c++) { out[added++] = c; bm_set(bm, c); }
  } else if (strcmp(name, "upper") == 0) {
    for (int c = 'A'; c <= 'Z'; c++) { out[added++] = c; bm_set(bm, c); }
  } else if (strcmp(name, "lower") == 0) {
    for (int c = 'a'; c <= 'z'; c++) { out[added++] = c; bm_set(bm, c); }
  } else if (strcmp(name, "space") == 0) {
    const char *spc = " \t\n\r\v\f";
    for (const char *p = spc; *p; p++) {
      out[added++] = *p;
      bm_set(bm, (unsigned char)*p);
    }
  } else if (strcmp(name, "alnum") == 0) {
    for (int c = '0'; c <= '9'; c++) { out[added++] = c; bm_set(bm, c); }
    for (int c = 'A'; c <= 'Z'; c++) { out[added++] = c; bm_set(bm, c); }
    for (int c = 'a'; c <= 'z'; c++) { out[added++] = c; bm_set(bm, c); }
  } else {
    return -1;
  }
  return added;
}

/* Expand SET into `out_chars` (positional) and `bm` (membership).
 * Returns number of chars written, or -1 on parse error. */
static int parse_set(const char *s, char *out_chars, bitmap_t bm) {
  int n = 0;
  while (*s) {
    if (*s == '\\' && s[1]) {
      char c;
      switch (s[1]) {
        case 'n':  c = '\n'; break;
        case 't':  c = '\t'; break;
        case 'r':  c = '\r'; break;
        case '\\': c = '\\'; break;
        case '\'': c = '\''; break;
        case '"':  c = '"';  break;
        default:   return -1;
      }
      out_chars[n++] = c;
      bm_set(bm, (unsigned char)c);
      s += 2;
    } else if (*s == '[' && s[1] == ':') {
      const char *end = s + 2;
      while (*end && !(*end == ':' && end[1] == ']')) end++;
      if (!*end) return -1;
      char name[16];
      int nlen = (int)(end - (s + 2));
      if (nlen <= 0 || nlen >= (int)sizeof(name)) return -1;
      for (int i = 0; i < nlen; i++) name[i] = s[2 + i];
      name[nlen] = '\0';
      int added = append_class(name, out_chars + n, bm);
      if (added < 0) return -1;
      n += added;
      s = end + 2;
    } else if (s[1] == '-' && s[2]) {
      unsigned char lo = (unsigned char)*s;
      unsigned char hi = (unsigned char)s[2];
      if (hi < lo) return -1;
      for (int c = lo; c <= hi; c++) {
        out_chars[n++] = (char)c;
        bm_set(bm, (unsigned char)c);
      }
      s += 3;
    } else {
      out_chars[n++] = *s;
      bm_set(bm, (unsigned char)*s);
      s++;
    }
  }
  return n;
}

static void usage(void) {
  uc_eputs(
      "Usage: tr SET1 SET2\n"
      "       tr -d SET1\n"
      "Reads stdin, writes stdout.\n");
}

int main(int argc, char *argv[]) {
  int delete_mode = 0;
  int argi = 1;

  if (argi < argc && strcmp(argv[argi], "--help") == 0) {
    usage();
    return 0;
  }
  if (argi < argc && strcmp(argv[argi], "-d") == 0) {
    delete_mode = 1;
    argi++;
  }

  if (delete_mode ? (argi + 1 != argc) : (argi + 2 != argc)) {
    usage();
    return 1;
  }

  char set1_chars[256], set2_chars[256];
  bitmap_t set1_in, set2_in;
  memset(set1_in, 0, sizeof(set1_in));
  memset(set2_in, 0, sizeof(set2_in));

  int n1 = parse_set(argv[argi], set1_chars, set1_in);
  if (n1 < 0 || n1 == 0) {
    uc_eputs("tr: invalid or empty SET1\n");
    return 1;
  }

  int n2 = 0;
  if (!delete_mode) {
    n2 = parse_set(argv[argi + 1], set2_chars, set2_in);
    if (n2 < 0) {
      uc_eputs("tr: invalid SET2\n");
      return 1;
    }
    if (n2 == 0) {
      uc_eputs("tr: SET2 cannot be empty (without -d)\n");
      return 1;
    }
  }

  /* Build translation table.  POSIX: if SET2 is shorter than SET1,
   * the last char of SET2 pads the remaining positions. */
  unsigned char tr_table[256];
  for (int i = 0; i < 256; i++) tr_table[i] = (unsigned char)i;
  if (!delete_mode) {
    for (int i = 0; i < n1; i++) {
      int j = (i < n2) ? i : n2 - 1;
      tr_table[(unsigned char)set1_chars[i]] = (unsigned char)set2_chars[j];
    }
  }

  char buf[256];
  ssize_t n;
  while ((n = read(0, buf, sizeof(buf))) > 0) {
    char out[256];
    int olen = 0;
    for (ssize_t i = 0; i < n; i++) {
      unsigned char c = (unsigned char)buf[i];
      if (delete_mode) {
        if (bm_get(set1_in, c)) continue;
        out[olen++] = (char)c;
      } else {
        out[olen++] = (char)tr_table[c];
      }
    }
    ssize_t off = 0;
    while (off < olen) {
      ssize_t w = write(1, out + off, olen - off);
      if (w <= 0) return 1;
      off += w;
    }
  }
  return (n < 0) ? 1 : 0;
}
