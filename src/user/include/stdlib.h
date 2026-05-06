/*
 * <stdlib.h> — general utilities (allocation, conversions, environment,
 * sorting / searching).
 *
 * POSIX subset; allocator entries live in src/user/lib/alloc.c, the
 * rest in src/user/lib/stdlib.c.
 *
 * malloc lazily acquires heap space from brk() only if the process did
 * not call uc_heap_init(). Applications that pre-seed a caller-owned
 * arena with uc_heap_init() keep the fixed-pool behavior.
 */

#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void free(void *ptr);

int atoi(const char *s);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);

int abs(int x);
long labs(long x);

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

char *getenv(const char *name);

void abort(void) __attribute__((noreturn));
void exit(int status) __attribute__((noreturn));
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff
int rand(void);
void srand(unsigned int seed);

#endif /* _STDLIB_H */
