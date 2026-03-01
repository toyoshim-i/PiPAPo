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

#endif /* PPAP_FD_TTY_H */
