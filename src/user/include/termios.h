/*
 * <termios.h> — terminal I/O attributes + tcgetattr/tcsetattr.
 *
 * Constants and `struct termios` come from src/common/termios.h.  The
 * tcgetattr / tcsetattr wrappers forward to ioctl() with the matching
 * TCGETS / TCSETS{,W,F} requests.
 */

#ifndef _TERMIOS_H
#define _TERMIOS_H

#include "common/termios.h"

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int actions, const struct termios *t);

#endif /* _TERMIOS_H */
