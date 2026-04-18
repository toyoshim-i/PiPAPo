/*
 * termios.h — host shadow of src/common/termios.h
 *
 * pi.h and push_line.c pull this in directly for struct termios / winsize
 * plus ioctl codes.  On host we use libc's native types and just make
 * sure the Linux ioctl codes (TCGETS, TCSETS, TIOCGWINSZ) are visible.
 */

#ifndef PPAP_TARGET_HOST_INCLUDE_COMMON_TERMIOS_H
#define PPAP_TARGET_HOST_INCLUDE_COMMON_TERMIOS_H

#include <sys/ioctl.h>
#include <termios.h>

#endif /* PPAP_TARGET_HOST_INCLUDE_COMMON_TERMIOS_H */
