/*
 * string.c — Minimal string/memory functions for freestanding m68k
 *
 * GCC on m68k emits calls to these even with __builtin_memset/memcpy
 * because the 68000 lacks block-copy instructions, so GCC prefers
 * to call the library version for non-trivial sizes.
 *
 * Also provides strlen, strcmp, strncmp for VFS/FS code that uses
 * <string.h> functions directly (rather than __builtin_ variants).
 */

#include <stddef.h>
#include <stdint.h>

void *memset(void *s, int c, size_t n) {
  uint8_t *p = (uint8_t *)s;
  while (n--) *p++ = (uint8_t)c;
  return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;
  while (n--) *d++ = *s++;
  return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;
  if (d < s) {
    while (n--) *d++ = *s++;
  } else {
    d += n;
    s += n;
    while (n--) *--d = *--s;
  }
  return dest;
}

size_t strlen(const char *s) {
  const char *p = s;
  while (*p) p++;
  return (size_t)(p - s);
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
  while (n && *a && *a == *b) {
    a++;
    b++;
    n--;
  }
  return n ? ((int)(unsigned char)*a - (int)(unsigned char)*b) : 0;
}

char *strcpy(char *dest, const char *src) {
  char *d = dest;
  while ((*d++ = *src++));
  return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
  char *d = dest;
  while (n && (*d++ = *src++)) n--;
  while (n--) *d++ = '\0';
  return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
  const uint8_t *a = (const uint8_t *)s1;
  const uint8_t *b = (const uint8_t *)s2;
  while (n--) {
    if (*a != *b) return (int)*a - (int)*b;
    a++;
    b++;
  }
  return 0;
}
