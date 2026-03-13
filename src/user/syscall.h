/*
 * syscall.h — User-space syscall wrappers for PPAP
 *
 * Declarations for the SVC/TRAP stubs defined in syscall.S.
 * No libc — uses only freestanding compiler headers.
 *
 * Shared ABI types and constants are in src/common/.
 */

#ifndef PPAP_USER_SYSCALL_H
#define PPAP_USER_SYSCALL_H

#include <stdint.h>
#include <stddef.h>

/* ── Shared ABI types and constants ────────────────────────────────────── */

#include "common/fcntl.h"
#include "common/seek.h"
#include "common/stat.h"
#include "common/dirent.h"
#include "common/poll.h"
#include "common/iovec.h"
#include "common/wait.h"
#include "common/ptrace.h"

/* ── Types ─────────────────────────────────────────────────────────────── */

typedef int32_t ssize_t;
typedef int32_t pid_t;

/* ── Process management ──────────────────────────────────────────────── */

void  _exit(int status) __attribute__((noreturn));
pid_t getpid(void);
pid_t getppid(void);
pid_t vfork(void);
int   execve(const char *path, char *const argv[], char *const envp[]);
pid_t waitpid(pid_t pid, int *status, int options);
long  ptrace(int req, pid_t pid, void *addr, void *data);
pid_t setsid(void);
int   setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
int   ioctl(int fd, unsigned long cmd, void *arg);

/* ── File I/O ────────────────────────────────────────────────────────── */

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int     open(const char *path, int flags, int mode);
int     close(int fd);
int     pipe(int fds[2]);
int     dup(int oldfd);
int     dup2(int oldfd, int newfd);

/* ── Scatter/gather I/O ──────────────────────────────────────────────── */

ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
ssize_t readv(int fd, const struct iovec *iov, int iovcnt);

/* ── File system ─────────────────────────────────────────────────────── */

int     lseek(int fd, int offset, int whence);
int     getcwd(char *buf, size_t size);
int     chdir(const char *path);
int     access(const char *path, int mode);
int     mkdir(const char *path, int mode);
int     unlink(const char *path);
int     rmdir(const char *path);
int     stat(const char *path, struct stat *buf);
int     getdents(int fd, struct dirent *buf, size_t count);

/* ── Memory management ──────────────────────────────────────────────── */

void *brk(void *addr);

/* ── Signals ───────────────────────────────────────────────────────── */

int kill(pid_t pid, int sig);
int sigaction(int sig, void *handler, void *old_handler);

/* ── Sleep ─────────────────────────────────────────────────────────── */

int nanosleep(const void *req, void *rem);

/* ── Time ──────────────────────────────────────────────────────────── */

/* clock_gettime maps to SYS_CLOCK_GETTIME32 (0x0501).
 * tp must point to struct { long tv_sec; long tv_nsec; }. */
int clock_gettime(int clk_id, void *tp);

/* ── Poll ──────────────────────────────────────────────────────────── */

int ppoll(struct pollfd *fds, int nfds, void *timeout,
          void *sigmask, int sigsetsize);

#endif /* PPAP_USER_SYSCALL_H */
