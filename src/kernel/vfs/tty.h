/*
 * tty.h — Multi-TTY driver public interface
 *
 * Supports multiple independent TTY instances:
 *   tty_devs[0] — /dev/ttyS0 (serial console)
 *   tty_devs[1] — /dev/tty1  (display console: fbcon or BIOS)
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

#ifndef PPAP_KERNEL_VFS_TTY_H
#define PPAP_KERNEL_VFS_TTY_H

#include "kernel/vfs/file.h"

/* TTY instance indices */
#define TTY_MAX 2
#define TTY_SERIAL 0  /* /dev/ttyS0 — serial console */
#define TTY_DISPLAY 1 /* /dev/tty1  — display console */

extern struct file tty_stdin;
extern struct file tty_stdout;
extern struct file tty_stderr;

/* File operations table — shared by all TTY instances */
extern const struct file_ops tty_fops;

/* I/O backend descriptor — all fields required unless noted */
typedef struct {
  /* Enqueue one byte.  Returns 1 on success, 0 if full.
   * If 0 and notify != NULL, the backend registers notify atomically
   * and calls it (from ISR) when space becomes available. */
  int (*putc)(char c, void (*notify)(void));
  void (*flush)(void);   /* flush output (NULL if not needed)  */
  int (*getc)(void);     /* read one character (-1 if none)    */
  int (*rx_avail)(void); /* non-zero if input available        */
  int (*get_cols)(void); /* terminal width  (NULL → default)   */
  int (*get_rows)(void); /* terminal height (NULL → default)   */
  void (*set_winsize)(int cols, int rows); /* resize (NULL → fixed) */
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

/* Check in_avail() on all TTY instances and wake blocked readers.
 * Called from vfs_notify(VFS_EVENT_IDLE) in the idle loop. */
void tty_poll_input(void);

/* Called from UART ISR when Ctrl-C (0x03) is received on instance idx.
 * Echoes ^C and sends SIGINT when ISIG is enabled.
 * Returns 1 if the character was consumed, 0 otherwise. */
int tty_signal_intr(int idx);

/* Set the foreground process group for TTY instance idx. */
void tty_set_fg_pgrp(int idx, int pgid);

/* Return non-zero if TTY instance idx has an I/O backend registered. */
int tty_backend_ready(int idx);

/* Set the console TTY instance used by fd_stdio_init().
 * Call from target_late_init() before the first process is spawned.
 * Default: TTY_SERIAL (0).  pico1calc and x68k use TTY_DISPLAY (1). */
void tty_set_console(int idx);

/* Raw byte I/O against the registered backend, bypassing line
 * discipline.  Used by devfs to back /dev/ttyS0 / /dev/tty1 read/
 * write paths without dragging in the UART driver header.  All return
 * -1 (read) / 0 (write) when no backend is registered for idx. */
int tty_raw_getc(int idx);
int tty_raw_putc(int idx, char c, void (*notify)(void));

/* Return an opaque pointer to the console TTY device (as set by
 * tty_set_console). Used by fd_stdio_init() to point stdin/stdout/stderr at the
 * primary console. */
void *tty_get_console_dev(void);

#endif /* PPAP_KERNEL_VFS_TTY_H */
