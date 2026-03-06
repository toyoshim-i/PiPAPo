/*
 * tty.h — UART tty driver public interface
 *
 * Provides three static struct file objects pre-wired to the UART:
 *   tty_stdin  — read-only,  backed by uart_getc()
 *   tty_stdout — write-only, backed by uart_putc()
 *   tty_stderr — write-only, backed by uart_putc()
 *
 * These are shared across processes (refcnt = 1 at boot; dup/fork may
 * increment it).  A single tty_fops vtable handles all three.
 *
 * fd_stdio_init() (fd.c) points fd_table[0/1/2] at these objects.
 */

#ifndef PPAP_FD_TTY_H
#define PPAP_FD_TTY_H

#include "file.h"

extern struct file tty_stdin;
extern struct file tty_stdout;
extern struct file tty_stderr;

/* Called from UART ISR when RX data arrives — wakes processes blocked on tty */
void tty_rx_notify(void);

/* Called from UART ISR when Ctrl-C (0x03) is received — delivers SIGINT.
 * Echoes ^C and sends the signal when ISIG is enabled.
 * Returns 1 if the character was consumed (ISIG on), 0 otherwise.
 * When consumed, the ISR should NOT put 0x03 in the RX ring buffer. */
int tty_signal_intr(void);

/* Set the foreground process group (for Ctrl-C / SIGINT delivery).
 * Must be called after init is launched so tty_fg_pgrp matches
 * the pgid of user processes. */
void tty_set_fg_pgrp(int pgid);

#endif /* PPAP_FD_TTY_H */
