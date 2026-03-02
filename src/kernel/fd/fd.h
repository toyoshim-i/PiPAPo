/*
 * fd.h — File descriptor table helpers
 *
 * fd_stdio_init  — wire fd 0/1/2 to the UART tty driver at boot
 * fd_alloc       — allocate the next free fd slot and bind a struct file
 * fd_free        — release a fd slot (decrements refcnt, calls close if 0)
 * fd_get         — look up a struct file * from a raw fd number
 *
 * All functions operate on the fd_table[] array inside a PCB.
 * Syscall wrappers (sys_io.c) bypass these helpers for direct fd validation;
 * these are for internal kernel use (process creation, dup, pipe).
 */

#ifndef PPAP_FD_FD_H
#define PPAP_FD_FD_H

#include "../proc/proc.h"
#include "file.h"

/*
 * Wire fd 0 (stdin), 1 (stdout), 2 (stderr) to the UART tty driver.
 * Must be called for every new process before it can use stdio.
 * Called from kmain() for proc_table[0]; child processes inherit via fork().
 */
void fd_stdio_init(pcb_t *p);

/*
 * Allocate the lowest free fd slot in p->fd_table[], bind f, and
 * increment f->refcnt.  Returns the allocated fd number, or -EMFILE
 * if all FD_MAX slots are in use.
 */
int fd_alloc(pcb_t *p, struct file *f);

/*
 * Release fd slot fd in p->fd_table[].  Decrements f->refcnt; if it
 * reaches zero, calls f->ops->close(f).  No-op if fd is out of range
 * or already NULL.
 */
void fd_free(pcb_t *p, int fd);

/*
 * Return the struct file * bound to fd in p, or NULL if fd is invalid
 * or unbound.
 */
struct file *fd_get(pcb_t *p, int fd);

/*
 * Inherit file descriptors from parent to child (for vfork).
 * Copies all fd_table entries and increments refcnt on each file.
 */
void fd_inherit(pcb_t *child, const pcb_t *parent);

/*
 * Close all open file descriptors for the given process.
 * Called from sys_exit() and sys_execve() during cleanup.
 */
void fd_close_all(pcb_t *p);

#endif /* PPAP_FD_FD_H */
