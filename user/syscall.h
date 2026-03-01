/*
 * syscall.h — User-space syscall wrappers for PPAP
 *
 * Declarations for the SVC stubs defined in syscall.S.
 * No libc — uses only freestanding compiler headers.
 */

#ifndef PPAP_USER_SYSCALL_H
#define PPAP_USER_SYSCALL_H

#include <stdint.h>
#include <stddef.h>

typedef int32_t ssize_t;
typedef int32_t pid_t;

/* ── Process management ──────────────────────────────────────────────────── */

void  _exit(int status) __attribute__((noreturn));
pid_t getpid(void);
pid_t vfork(void);
int   execve(const char *path, char *const argv[], char *const envp[]);
pid_t waitpid(pid_t pid, int *status, int options);

/* ── File I/O ────────────────────────────────────────────────────────────── */

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int     open(const char *path, int flags, int mode);
int     close(int fd);
int     pipe(int fds[2]);

#endif /* PPAP_USER_SYSCALL_H */
