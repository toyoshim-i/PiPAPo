/*
 * <sys/ioctl.h> — ioctl() prototype + terminal-related request macros.
 *
 * The terminal request numbers and `struct winsize` come from
 * src/common/termios.h (kernel/user shared).
 */

#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#include "common/termios.h"

int ioctl(int fd, unsigned long cmd, void *arg);

#endif /* _SYS_IOCTL_H */
