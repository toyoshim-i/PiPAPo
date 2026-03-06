/*
 * tty.h — Multi-TTY driver public interface
 *
 * Supports multiple independent TTY instances:
 *   tty_devs[0] — /dev/ttyS0 (UART, always available)
 *   tty_devs[1] — /dev/tty1  (fbcon+kbd on PicoCalc)
 *
 * Three static struct file objects for the default console:
 *   tty_stdin  — read-only
 *   tty_stdout — write-only
 *   tty_stderr — write-only
 *
 * These default objects have priv = &tty_devs[0] (set in fd_stdio_init).
 * When getty/askfirst opens /dev/ttyXX, sys_fs.c allocates a new file
 * from file_pool with priv = tty_get_dev(idx).
 */

#ifndef PPAP_FD_TTY_H
#define PPAP_FD_TTY_H

#include "file.h"

/* TTY instance indices */
#define TTY_MAX     2
#define TTY_SERIAL  0   /* /dev/ttyS0 — UART */
#define TTY_DISPLAY 1   /* /dev/tty1  — fbcon+kbd */

extern struct file tty_stdin;
extern struct file tty_stdout;
extern struct file tty_stderr;

/* File operations table — shared by all TTY instances */
extern const struct file_ops tty_fops;

/* I/O backend descriptor — all fields required */
typedef struct {
    void (*putc)(char c);       /* write one character                */
    void (*flush)(void);        /* flush output (NULL if not needed)  */
    int  (*getc)(void);         /* read one character (-1 if none)    */
    int  (*rx_avail)(void);     /* non-zero if input available        */
    int  (*get_cols)(void);     /* terminal width  (NULL → default)   */
    int  (*get_rows)(void);     /* terminal height (NULL → default)   */
} tty_backend_t;

/* Set the I/O backend for TTY instance idx.
 * idx=0: ttyS0 (UART), idx=1: tty1 (display). */
void tty_set_backend(int idx, const tty_backend_t *be);

/* Return an opaque pointer to the tty_dev_t for instance idx.
 * Used by sys_fs.c to set file->priv when opening TTY devices. */
void *tty_get_dev(int idx);

/* Called from ISR / scheduler when RX data arrives on instance idx.
 * Wakes processes blocked on that TTY. */
void tty_rx_notify(int idx);

/* Called from UART ISR when Ctrl-C (0x03) is received on instance idx.
 * Echoes ^C and sends SIGINT when ISIG is enabled.
 * Returns 1 if the character was consumed, 0 otherwise. */
int tty_signal_intr(int idx);

/* Set the foreground process group for TTY instance idx. */
void tty_set_fg_pgrp(int idx, int pgid);

#endif /* PPAP_FD_TTY_H */
