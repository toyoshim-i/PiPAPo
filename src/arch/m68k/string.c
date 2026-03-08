/*
 * string.c — Minimal memset/memcpy/memmove for freestanding m68k
 *
 * GCC on m68k emits calls to these even with __builtin_memset/memcpy
 * because the 68000 lacks block-copy instructions, so GCC prefers
 * to call the library version for non-trivial sizes.
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

void *memcpy(void *dest, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--)
        *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dest;
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
    return dest;
}
