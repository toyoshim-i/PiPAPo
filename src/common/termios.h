/*
 * termios.h --- Terminal I/O constants and structures
 *
 * Shared between kernel and user space.
 * Layout matches Linux ARM (used by musl libc).
 */

#ifndef PPAP_COMMON_TERMIOS_H
#define PPAP_COMMON_TERMIOS_H

#include <stdint.h>

/* ── ioctl request codes ───────────────────────────────────────────────── */

#define TCGETS 0x5401u
#define TCSETS 0x5402u
#define TCSETSW 0x5403u
#define TCSETSF 0x5404u
#define TIOCGWINSZ 0x5413u
#define TIOCSWINSZ 0x5414u

/* ── c_iflag bits ──────────────────────────────────────────────────────── */

#define ICRNL 0x0100u /* map CR to NL on input */
#define IXON 0x0400u  /* enable XON/XOFF output control */

/* ── c_oflag bits ──────────────────────────────────────────────────────── */

#define OPOST 0x0001u /* post-process output */
#define ONLCR 0x0004u /* map NL to CR-NL on output */

/* ── c_lflag bits ──────────────────────────────────────────────────────── */

#define ISIG 0x0001u   /* enable signals (INTR, QUIT, etc.) */
#define ICANON 0x0002u /* canonical (line) mode */
#define ECHO 0x0008u   /* echo input characters */

/* ── Control character count ───────────────────────────────────────────── */

#define NCCS 19 /* number of control characters (matches Linux ARM) */

/* ── termios structure ─────────────────────────────────────────────────── */

struct termios {
  uint32_t c_iflag;
  uint32_t c_oflag;
  uint32_t c_cflag;
  uint32_t c_lflag;
  uint8_t c_line;
  uint8_t c_cc[NCCS];
};

/* ── Window size ───────────────────────────────────────────────────────── */

struct winsize {
  uint16_t ws_row;
  uint16_t ws_col;
  uint16_t ws_xpixel;
  uint16_t ws_ypixel;
};

#endif /* PPAP_COMMON_TERMIOS_H */
