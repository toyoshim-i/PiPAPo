/*
 * string.h — Minimal freestanding string functions for m68k
 *
 * Provides declarations for functions implemented in arch/m68k/string.c.
 * This header is used instead of the system <string.h> which is not
 * available in the freestanding m68k cross-compilation environment.
 */

#ifndef PPAP_M68K_STRING_H
#define PPAP_M68K_STRING_H

#include <stddef.h>

void  *memset(void *s, int c, size_t n);
void  *memcpy(void *dest, const void *src, size_t n);
void  *memmove(void *dest, const void *src, size_t n);
int    memcmp(const void *s1, const void *s2, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dest, const char *src);
char  *strncpy(char *dest, const char *src, size_t n);

#endif /* PPAP_M68K_STRING_H */
