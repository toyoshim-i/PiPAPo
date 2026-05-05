/*
 * <stdio.h> — formatted I/O.
 *
 * POSIX subset; implementations live in src/user/lib/stdio.c.
 * FILE-stream APIs (fopen / fread / fprintf / fputs / fgets / …) are
 * not yet provided — see the uc_* fallbacks in <lib/uclib.h> for
 * stderr-targeted output.
 */

#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stddef.h>

int putchar(int c);

int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int printf(const char *fmt, ...);

#endif /* _STDIO_H */
