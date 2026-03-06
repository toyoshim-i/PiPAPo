/*
 * tty.h — TTY driver public interface
 *
 * Provides three static struct file objects for the system console:
 *   tty_stdin  — read-only
 *   tty_stdout — write-only
 *   tty_stderr — write-only
 *
 * By default, I/O is backed by UART (uart_putc / uart_getc).
 * Call tty_set_backend() to switch to an alternate backend (e.g.
 * fbcon + keyboard on PicoCalc).
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

/* I/O backend descriptor — all fields required */
typedef struct {
    void (*putc)(char c);       /* write one character                */
    void (*flush)(void);        /* flush output (NULL if not needed)  */
    int  (*getc)(void);         /* read one character (-1 if none)    */
    int  (*rx_avail)(void);     /* non-zero if input available        */
    int  (*get_cols)(void);     /* terminal width  (NULL → default)   */
    int  (*get_rows)(void);     /* terminal height (NULL → default)   */
} tty_backend_t;

/* Switch the tty I/O backend.  Must be called before processes start. */
void tty_set_backend(const tty_backend_t *be);

/* Called from ISR / scheduler when RX data arrives — wakes blocked tty readers */
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
