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
#include <stdlib.h>
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

char *strstr(const char *haystack, const char *needle) {
  size_t nlen = strlen(needle);
  if (nlen == 0) return (char *)haystack;
  size_t hlen = strlen(haystack);
  if (hlen < nlen) return (void *)0;
  for (size_t i = 0; i <= hlen - nlen; i++) {
    if (memcmp(haystack + i, needle, nlen) == 0) return (char *)haystack + i;
  }
  return (void *)0;
}

size_t strspn(const char *s, const char *accept) {
  size_t n = 0;
  for (; s[n]; n++) {
    if (!strchr(accept, (unsigned char)s[n])) break;
  }
  return n;
}

size_t strcspn(const char *s, const char *reject) {
  size_t n = 0;
  for (; s[n]; n++) {
    if (strchr(reject, (unsigned char)s[n])) break;
  }
  return n;
}

char *strpbrk(const char *s, const char *accept) {
  for (; *s; s++) {
    if (strchr(accept, (unsigned char)*s)) return (char *)s;
  }
  return (void *)0;
}

char *strdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = malloc(n);
  if (p) memcpy(p, s, n);
  return p;
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
