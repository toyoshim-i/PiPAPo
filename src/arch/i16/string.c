/*
 * string.c — Minimal string/memory functions for i16 bare-metal
 *
 * ia16-elf-gcc with -nostdlib has no libc, so we provide these.
 */

#include <stddef.h>
#include <stdint.h>

void *memset(void *s, int c, size_t n)
{
  uint8_t *p = (uint8_t *)s;
  while (n--)
    *p++ = (uint8_t)c;
  return s;
}

void *memcpy(void *dst, const void *src, size_t n)
{
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  while (n--)
    *d++ = *s++;
  return dst;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
  const uint8_t *a = (const uint8_t *)s1;
  const uint8_t *b = (const uint8_t *)s2;
  while (n--) {
    if (*a != *b)
      return *a - *b;
    a++;
    b++;
  }
  return 0;
}

void *memmove(void *dst, const void *src, size_t n)
{
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  if (d < s) {
    while (n--)
      *d++ = *s++;
  } else {
    d += n;
    s += n;
    while (n--)
      *--d = *--s;
  }
  return dst;
}

size_t strlen(const char *s)
{
  const char *p = s;
  while (*p)
    p++;
  return (size_t)(p - s);
}

int strcmp(const char *s1, const char *s2)
{
  while (*s1 && *s1 == *s2) {
    s1++;
    s2++;
  }
  return (uint8_t)*s1 - (uint8_t)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
  while (n && *s1 && *s1 == *s2) {
    s1++;
    s2++;
    n--;
  }
  return n ? (uint8_t)*s1 - (uint8_t)*s2 : 0;
}

char *strcpy(char *dst, const char *src)
{
  char *d = dst;
  while ((*d++ = *src++))
    ;
  return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
  char *d = dst;
  while (n && (*d++ = *src++))
    n--;
  while (n--)
    *d++ = '\0';
  return dst;
}

char *strcat(char *dst, const char *src)
{
  char *d = dst;
  while (*d)
    d++;
  while ((*d++ = *src++))
    ;
  return dst;
}

char *strchr(const char *s, int c)
{
  while (*s) {
    if (*s == (char)c)
      return (char *)s;
    s++;
  }
  return (c == '\0') ? (char *)s : (char *)0;
}

char *strrchr(const char *s, int c)
{
  const char *last = (const char *)0;
  while (*s) {
    if (*s == (char)c)
      last = s;
    s++;
  }
  if (c == '\0')
    return (char *)s;
  return (char *)last;
}
