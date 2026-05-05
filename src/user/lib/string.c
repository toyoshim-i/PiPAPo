/*
 * string.c — string and memory operations.
 *
 * POSIX-named subset of <string.h>.  Implementations are deliberately
 * compact (no SIMD, no alignment tricks) — PPAP user binaries link
 * with -ffunction-sections so unused entries are dropped at gc-sections
 * time.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    if (!a[i]) return 0;
  }
  return 0;
}

char *strcpy(char *dst, const char *src) {
  char *d = dst;
  while ((*d++ = *src++))
    ;
  return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
  size_t i;
  for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
  for (; i < n; i++) dst[i] = '\0';
  return dst;
}

char *strchr(const char *s, int c) {
  for (; *s; s++)
    if (*s == (char)c) return (char *)s;
  return (c == 0) ? (char *)s : (void *)0;
}

char *strrchr(const char *s, int c) {
  const char *last = (void *)0;
  for (; *s; s++)
    if (*s == (char)c) last = s;
  if (c == 0) return (char *)s;
  return (char *)last;
}

void *memcpy(void *dst, const void *src, size_t n) {
  char *d = dst;
  const char *s = src;
  for (size_t i = 0; i < n; i++) d[i] = s[i];
  return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
  unsigned char *d = dst;
  const unsigned char *s = src;
  if (d == s || n == 0) return dst;
  if (d < s) {
    for (size_t i = 0; i < n; i++) d[i] = s[i];
  } else {
    while (n > 0) {
      n--;
      d[n] = s[n];
    }
  }
  return dst;
}

void *memset(void *dst, int c, size_t n) {
  char *d = dst;
  for (size_t i = 0; i < n; i++) d[i] = (char)c;
  return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *p = a, *q = b;
  for (size_t i = 0; i < n; i++)
    if (p[i] != q[i]) return p[i] - q[i];
  return 0;
}
