/*
 * <stdlib.h> — general utilities (allocation, conversions, environment).
 *
 * POSIX subset; implementations live in src/user/lib/stdlib.c.
 * Numeric conversion (strtol, strtoul), sorting (qsort), and the
 * remaining standard surface will be added in a later milestone.
 *
 * The malloc allocator must be seeded with a caller-owned static
 * pool via uc_heap_init() (declared in <lib/uclib.h>) before the
 * first malloc call.
 */

#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void free(void *ptr);

int atoi(const char *s);
char *getenv(const char *name);

#endif /* _STDLIB_H */
