/*
 * errno.h — POSIX error codes used by the kernel syscall layer
 *
 * Only the values needed by Phase 1 are listed here.  The numbers match
 * the Linux/ARM EABI ABI so that musl libc user programs interpret them
 * correctly when returned as negative values from syscalls.
 */

#ifndef PPAP_ERRNO_H
#define PPAP_ERRNO_H

#define EBADF    9   /* Bad file descriptor                */
#define EINVAL  22   /* Invalid argument                   */
#define EMFILE  24   /* Too many open files                */
#define ENOSYS  38   /* Function not implemented           */

#endif /* PPAP_ERRNO_H */
