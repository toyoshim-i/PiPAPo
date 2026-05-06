/*
 * <sys/stat.h> — struct stat, file-mode macros, and stat / lstat /
 * mkdir / chmod entry points.
 *
 * Constants and `struct stat` come from src/common/stat.h.
 */

#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include "common/stat.h"

int stat(const char *path, struct stat *buf);
int lstat(const char *path, struct stat *buf);
int mkdir(const char *path, int mode);
int chmod(const char *path, int mode);

#endif /* _SYS_STAT_H */
