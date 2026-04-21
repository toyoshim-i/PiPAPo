/*
 * reset.c — restore a sane terminal state after a crashed subsystem
 *
 * PPAP subsystems (Human68k, MS-DOS, CP/M, S-OS) rewrite termios
 * flags and the emulator's escape-sequence state machine to match
 * each guest OS's de-facto terminal behavior.  When the guest
 * process crashes mid-session it can leave the host tty in a
 * confused state: echo off, canonical mode off, alternate
 * character set selected, scroll region clipped, reverse video
 * stuck, cursor hidden.
 *
 * `reset` restores the POSIX/VT100 defaults so an interactive
 * shell is usable again:
 *
 *   1. Emits ANSI/VT100 reset sequences to stdout.  Matches the
 *      sequence that ncurses / busybox reset(1) send.
 *   2. Resets termios flags on stdin (ICANON | ECHO | ISIG, etc.)
 *      via ioctl(0, TCSETS, &t) — like `stty sane`.
 *
 * No options other than --help.
 */

#include "common/termios.h"
#include "lib/uclib.h"

int main(int argc, char *argv[]) {
  if (argc > 1 && uc_strcmp(argv[1], "--help") == 0) {
    uc_puts(
        "Usage: reset\n"
        "Restore terminal to sane defaults (termios + VT100 state).\n");
    return 0;
  }

  /* Display reset.  See `man 4 console_codes`:
   *   ESC c       full reset (RIS)
   *   ESC ( B     select G0 = US ASCII
   *   ESC [ m     reset display attributes (SGR 0)
   *   ESC [ r     clear scroll region
   *   ESC [ J     erase from cursor to end of screen
   *   ESC [ ?25h  show cursor
   */
  static const char seq[] =
      "\033c"
      "\033(B"
      "\033[m"
      "\033[r"
      "\033[J"
      "\033[?25h";
  write(1, seq, sizeof(seq) - 1);

  /* termios reset on stdin (the controlling tty).  Preserves
   * c_cflag and c_cc[] — user shells typically want their existing
   * baud rate and control characters left alone. */
  struct termios t;
  if (ioctl(0, TCGETS, &t) == 0) {
    t.c_iflag = ICRNL | IXON;
    t.c_oflag = OPOST | ONLCR;
    t.c_lflag = ISIG | ICANON | ECHO;
    ioctl(0, TCSETS, &t);
  }
  return 0;
}
