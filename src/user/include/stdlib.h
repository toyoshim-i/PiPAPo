/*
 * <stdlib.h> — general utilities (allocation, conversions, environment).
 *
 * Declarations only at this stage; implementations are not yet wired
 * in.  Numeric conversion (strtol), sorting (qsort), and the rest of
 * the standard surface will be added later.
 */

#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void free(void *ptr);

int atoi(const char *s);
char *getenv(const char *name);

void exit(int status) __attribute__((noreturn));

#endif /* _STDLIB_H */
