/*
 * <errno.h> — error number macros (EINVAL, ENOENT, …) and the errno
 * variable.
 *
 * Forwards the error-code macros from the kernel/user shared header.
 * `errno` is a plain global int (PPAP user space is single-threaded).
 *
 * Note: PPAP's syscall wrappers still return negative error codes
 * directly; they do not (yet) populate `errno`.  Code that needs to
 * inspect errno after a libc call (perror, fopen failure paths) will
 * see whatever the last writer set, which today is just the compat
 * layer in third-party ports — not the kernel.
 */

#ifndef _ERRNO_H
#define _ERRNO_H

#include "common/errno.h"

extern int errno;

/* POSIX __errno_location indirection used by some third-party headers. */
int *__errno_location(void);

#endif /* _ERRNO_H */
