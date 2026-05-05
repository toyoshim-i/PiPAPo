/*
 * <stdio.h> — formatted I/O.
 *
 * Declarations only at this stage; implementations are not yet wired
 * in.  FILE-stream APIs (fopen / fread / fprintf / …) will be added
 * later.
 */

#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stddef.h>

/* Character output. */
int putchar(int c);
int puts(const char *s);

/* Formatted output to a buffer. */
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* Formatted output to stdout / stderr (256-byte stack buffer). */
int printf(const char *fmt, ...);

#endif /* _STDIO_H */
