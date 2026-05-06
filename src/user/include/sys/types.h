/*
 * <sys/types.h> — POSIX system data types.
 *
 * Minimal subset for PPAP user space.  Sizes match what the kernel
 * syscalls expose: PIDs and ssize_t are 32-bit, off_t mirrors stat's
 * 32-bit timestamp / size scheme.
 */

#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef int32_t ssize_t;
typedef int32_t pid_t;
typedef int32_t off_t;
typedef int32_t mode_t;
typedef int32_t uid_t;
typedef int32_t gid_t;
typedef long time_t;
typedef uint32_t dev_t;
typedef uint32_t ino_t;
typedef uint32_t nlink_t;
typedef uint32_t blksize_t;
typedef uint32_t blkcnt_t;

#endif /* _SYS_TYPES_H */
