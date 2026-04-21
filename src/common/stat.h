/*
 * stat.h --- File mode constants and struct stat
 *
 * Shared between kernel and user space.
 * struct stat is the PPAP-native stat structure (4 x uint32_t),
 * NOT the Linux-compat stat64 used by musl.
 */

#ifndef PPAP_COMMON_STAT_H
#define PPAP_COMMON_STAT_H

#include <stdint.h>

/* File type mask and values (POSIX-compatible) */
#define S_IFMT 0170000  /* type-of-file mask                        */
#define S_IFDIR 0040000 /* directory                                 */
#define S_IFCHR 0020000 /* character special (device)                */
#define S_IFREG 0100000 /* regular file                              */
#define S_IFLNK 0120000 /* symbolic link                             */

#define S_ISDIR(m) (((m)&S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m)&S_IFMT) == S_IFREG)
#define S_ISLNK(m) (((m)&S_IFMT) == S_IFLNK)
#define S_ISCHR(m) (((m)&S_IFMT) == S_IFCHR)

/* PPAP-native stat structure.
 *
 * Timestamps are seconds since the Unix epoch, using the same clock as
 * clock_gettime(CLOCK_REALTIME).  Filesystems that do not store times
 * (romfs) or do not have a meaningful source (boot before any RTC seed)
 * report zero.  sub-second precision is intentionally not exposed here —
 * adding an st_*time_nsec field later is a separate ABI bump. */
struct stat {
  uint32_t st_ino;   /* inode number (FS-specific)                */
  uint32_t st_mode;  /* file type + permissions (S_IF* | 0755)    */
  uint32_t st_nlink; /* number of hard links (always 1 for romfs) */
  uint32_t st_size;  /* file size in bytes                        */
  uint32_t st_mtime; /* last modification time (seconds since epoch) */
  uint32_t st_ctime; /* last status change time (seconds since epoch) */
  uint32_t st_atime; /* last access time (seconds since epoch, 0 if unsupported) */
};

#endif /* PPAP_COMMON_STAT_H */
