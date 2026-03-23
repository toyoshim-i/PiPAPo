/*
 * string.c — Simple memcpy/memset/memmove for QEMU rv32
 *
 * The Pico SDK's toolchain provides memcpy compiled with Zcb
 * (compressed byte load/store) instructions that Hazard3 supports
 * but QEMU 8.x doesn't.  Override with simple byte-loop versions.
 */

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dest, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;
  while (n--) *d++ = *s++;
  return dest;
}

void *memset(void *dest, int c, size_t n) {
  uint8_t *d = (uint8_t *)dest;
  while (n--) *d++ = (uint8_t)c;
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
  while (*a && *a == *b) { a++; b++; }
  return *(const unsigned char *)a - *(const unsigned char *)b;
}

int strncmp(const char *a, const char *b, size_t n) {
  while (n && *a && *a == *b) { a++; b++; n--; }
  return n ? *(const unsigned char *)a - *(const unsigned char *)b : 0;
}

char *strcpy(char *dest, const char *src) {
  char *d = dest;
  while ((*d++ = *src++)) ;
  return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
  char *d = dest;
  while (n && (*d++ = *src++)) n--;
  while (n--) *d++ = '\0';
  return dest;
}

int memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *p = (const uint8_t *)a;
  const uint8_t *q = (const uint8_t *)b;
  while (n--) {
    if (*p != *q) return *p - *q;
    p++;
    q++;
  }
  return 0;
}
