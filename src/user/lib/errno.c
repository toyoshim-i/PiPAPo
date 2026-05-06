/*
 * errno.c — per-process errno storage.
 *
 * Single global int — PPAP user space is single-threaded, so no
 * thread-local indirection is needed.  __errno_location returns the
 * address; some libc-using third-party code (musl-style) calls it
 * directly through the `errno` macro.
 *
 * Syscall wrappers do NOT yet convert their negative-return convention
 * into errno + -1 / NULL — that conversion lands when a consumer
 * actually reads errno.
 */

#include <errno.h>

int errno = 0;

int *__errno_location(void) { return &errno; }
