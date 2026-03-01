/*
 * tty_stub.c — Host-side stubs for the UART tty driver
 *
 * Replaces src/kernel/fd/tty.c when building fd tests on the host.
 * tty.c contains __asm__ volatile("wfi") which is ARM-only.
 *
 * Provides the three extern struct file objects that fd.c's fd_stdio_init()
 * assigns to fd 0/1/2.  The ops pointer is NULL — fd tests only exercise
 * the bookkeeping in fd.c (slot assignment, refcnt, bounds checking), not
 * the actual read/write paths.
 */

#include "kernel/fd/file.h"

struct file tty_stdin  = { NULL, NULL, O_RDONLY, 1u };
struct file tty_stdout = { NULL, NULL, O_WRONLY, 1u };
struct file tty_stderr = { NULL, NULL, O_WRONLY, 1u };
