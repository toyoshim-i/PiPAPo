/*
 * <ctype.h> — character classification and case mapping.
 *
 * Header-only, ASCII-only implementation.  No locale support —
 * PPAP user space is single-locale (effectively "C").  Each entry
 * is a static inline so unused entries cost nothing.
 *
 * Per POSIX, the argument must be representable as `unsigned char`
 * or be EOF.  We rely on the compiler's promotion of the argument
 * and the comparisons treat the value as it is; pass through
 * `(unsigned char)c` at call sites if `char` may be signed in your
 * platform.
 */

#ifndef _CTYPE_H
#define _CTYPE_H

static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int isalpha(int c) { return isupper(c) || islower(c); }
static inline int isalnum(int c) { return isalpha(c) || isdigit(c); }
static inline int isxdigit(int c) {
  return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static inline int isspace(int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
         c == '\f';
}
static inline int isblank(int c) { return c == ' ' || c == '\t'; }
static inline int iscntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7f; }
static inline int isprint(int c) { return c >= 0x20 && c < 0x7f; }
static inline int isgraph(int c) { return c > 0x20 && c < 0x7f; }
static inline int ispunct(int c) { return isgraph(c) && !isalnum(c); }

static inline int toupper(int c) {
  return islower(c) ? c - ('a' - 'A') : c;
}
static inline int tolower(int c) {
  return isupper(c) ? c + ('a' - 'A') : c;
}

#endif /* _CTYPE_H */
