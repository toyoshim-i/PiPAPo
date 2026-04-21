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

#include <stddef.h>
#include <stdint.h>

/* ── Shared ABI types and constants ────────────────────────────────────── */

#include "common/dirent.h"
#include "common/fcntl.h"
#include "common/iovec.h"
#include "common/poll.h"
#include "common/ptrace.h"
#include "common/seek.h"
#include "common/stat.h"
#include "common/statfs.h"
#include "common/utsname.h"
#include "common/wait.h"

/* ── Types ─────────────────────────────────────────────────────────────── */

typedef int32_t ssize_t;
typedef int32_t pid_t;

#if defined(__xtensa__)
static inline long __attribute__((always_inline))
ppap_xtensa_syscall0(long nr) {
  register long a2 asm("a2") = 0;
  register long a7 asm("a7") = nr;
  asm volatile("ill" : "+r"(a2) : "r"(a7) : "memory");
  return a2;
}

static inline long __attribute__((always_inline))
ppap_xtensa_syscall1(long nr, long arg0) {
  register long a2 asm("a2") = arg0;
  register long a7 asm("a7") = nr;
  asm volatile("ill" : "+r"(a2) : "r"(a7) : "memory");
  return a2;
}

static inline long __attribute__((always_inline))
ppap_xtensa_syscall2(long nr, long arg0, long arg1) {
  register long a2 asm("a2") = arg0;
  register long a3 asm("a3") = arg1;
  register long a7 asm("a7") = nr;
  asm volatile("ill" : "+r"(a2) : "r"(a3), "r"(a7) : "memory");
  return a2;
}

static inline long __attribute__((always_inline))
ppap_xtensa_syscall3(long nr, long arg0, long arg1, long arg2) {
  register long a2 asm("a2") = arg0;
  register long a3 asm("a3") = arg1;
  register long a4 asm("a4") = arg2;
  register long a7 asm("a7") = nr;
  asm volatile("ill"
               : "+r"(a2)
               : "r"(a3), "r"(a4), "r"(a7)
               : "memory");
  return a2;
}

static inline long __attribute__((always_inline))
ppap_xtensa_syscall4(long nr, long arg0, long arg1, long arg2, long arg3) {
  register long a2 asm("a2") = arg0;
  register long a3 asm("a3") = arg1;
  register long a4 asm("a4") = arg2;
  register long a5 asm("a5") = arg3;
  register long a7 asm("a7") = nr;
  asm volatile("ill"
               : "+r"(a2)
               : "r"(a3), "r"(a4), "r"(a5), "r"(a7)
               : "memory");
  return a2;
}

static inline long __attribute__((always_inline))
ppap_xtensa_syscall5(long nr, long arg0, long arg1, long arg2, long arg3,
                     long arg4) {
  register long a2 asm("a2") = arg0;
  register long a3 asm("a3") = arg1;
  register long a4 asm("a4") = arg2;
  register long a5 asm("a5") = arg3;
  register long a6 asm("a6") = arg4;
  register long a7 asm("a7") = nr;
  asm volatile("ill"
               : "+r"(a2)
               : "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
               : "memory");
  return a2;
}

static inline __attribute__((always_inline, noreturn)) void _exit(int status) {
  (void)ppap_xtensa_syscall1(0x0000, status);
  for (;;)
    asm volatile("j ." ::: "memory");
}

static inline pid_t getpid(void) { return (pid_t)ppap_xtensa_syscall0(0x0006); }
static inline int uname(struct utsname *buf) {
  return (int)ppap_xtensa_syscall1(0x0B01, (long)buf);
}
static inline pid_t getppid(void) {
  return (pid_t)ppap_xtensa_syscall0(0x0008);
}
static inline pid_t vfork(void) { return (pid_t)ppap_xtensa_syscall0(0x0002); }
static inline int execve(const char *path, char *const argv[],
                         char *const envp[]) {
  return (int)ppap_xtensa_syscall3(0x0003, (long)path, (long)argv,
                                   (long)envp);
}
static inline pid_t waitpid(pid_t pid, int *status, int options) {
  return (pid_t)ppap_xtensa_syscall3(0x0004, pid, (long)status, options);
}
static inline long ptrace(int req, pid_t pid, void *addr, void *data) {
  return ppap_xtensa_syscall4(0x000F, req, pid, (long)addr, (long)data);
}
static inline pid_t setsid(void) { return (pid_t)ppap_xtensa_syscall0(0x000B); }
static inline int setpgid(pid_t pid, pid_t pgid) {
  return (int)ppap_xtensa_syscall2(0x0009, pid, pgid);
}
static inline pid_t getpgid(pid_t pid) {
  return (pid_t)ppap_xtensa_syscall1(0x000A, pid);
}
static inline int ioctl(int fd, unsigned long cmd, void *arg) {
  return (int)ppap_xtensa_syscall3(0x0107, fd, cmd, (long)arg);
}

static inline ssize_t read(int fd, void *buf, size_t n) {
  return (ssize_t)ppap_xtensa_syscall3(0x0100, fd, (long)buf, (long)n);
}
static inline ssize_t write(int fd, const void *buf, size_t n) {
  return (ssize_t)ppap_xtensa_syscall3(0x0101, fd, (long)buf, (long)n);
}
static inline int open(const char *path, int flags, int mode) {
  return (int)ppap_xtensa_syscall3(0x0102, (long)path, flags, mode);
}
static inline int close(int fd) {
  return (int)ppap_xtensa_syscall1(0x0103, fd);
}
static inline int pipe(int fds[2]) {
  return (int)ppap_xtensa_syscall1(0x0106, (long)fds);
}
static inline int dup(int oldfd) {
  return (int)ppap_xtensa_syscall1(0x0104, oldfd);
}
static inline int dup2(int oldfd, int newfd) {
  return (int)ppap_xtensa_syscall2(0x0105, oldfd, newfd);
}

static inline ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
  return (ssize_t)ppap_xtensa_syscall3(0x010A, fd, (long)iov, iovcnt);
}
static inline ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
  return (ssize_t)ppap_xtensa_syscall3(0x0109, fd, (long)iov, iovcnt);
}

static inline int lseek(int fd, int offset, int whence) {
  return (int)ppap_xtensa_syscall3(0x010B, fd, offset, whence);
}
static inline int getcwd(char *buf, size_t size) {
  return (int)ppap_xtensa_syscall2(0x0203, (long)buf, (long)size);
}
static inline int chdir(const char *path) {
  return (int)ppap_xtensa_syscall1(0x0207, (long)path);
}
static inline ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
  return (ssize_t)ppap_xtensa_syscall3(0x0208, (long)path, (long)buf,
                                       (long)bufsiz);
}
static inline int access(const char *path, int mode) {
  return (int)ppap_xtensa_syscall2(0x0202, (long)path, mode);
}
static inline int mkdir(const char *path, int mode) {
  return (int)ppap_xtensa_syscall2(0x0204, (long)path, mode);
}
static inline int unlink(const char *path) {
  return (int)ppap_xtensa_syscall1(0x0206, (long)path);
}
static inline int rmdir(const char *path) {
  return (int)ppap_xtensa_syscall1(0x0205, (long)path);
}
static inline int stat(const char *path, struct stat *buf) {
  return (int)ppap_xtensa_syscall2(0x0200, (long)path, (long)buf);
}
static inline int getdents(int fd, struct dirent *buf, size_t count) {
  return (int)ppap_xtensa_syscall3(0x0300, fd, (long)buf, (long)count);
}
static inline int statfs64(const char *path, long sz, struct statfs *buf) {
  return (int)ppap_xtensa_syscall3(0x0305, (long)path, sz, (long)buf);
}

static inline void *brk(void *addr) {
  return (void *)ppap_xtensa_syscall1(0x0400, (long)addr);
}

static inline int kill(pid_t pid, int sig) {
  return (int)ppap_xtensa_syscall2(0x0600, pid, sig);
}
static inline int sigaction(int sig, void *handler, void *old_handler) {
  return (int)ppap_xtensa_syscall3(0x0601, sig, (long)handler,
                                   (long)old_handler);
}

static inline int nanosleep(const void *req, void *rem) {
  return (int)ppap_xtensa_syscall2(0x0500, (long)req, (long)rem);
}
static inline int clock_gettime(int clk_id, void *tp) {
  return (int)ppap_xtensa_syscall2(0x0501, clk_id, (long)tp);
}

static inline int ppoll(struct pollfd *fds, int nfds, void *timeout,
                        void *sigmask, int sigsetsize) {
  return (int)ppap_xtensa_syscall5(0x0701, (long)fds, nfds, (long)timeout,
                                   (long)sigmask, sigsetsize);
}

static inline void poweroff(void) { ppap_xtensa_syscall0(0x0B00); }
#else
/* ── Process management ──────────────────────────────────────────────── */

void _exit(int status) __attribute__((noreturn));
pid_t getpid(void);
int uname(struct utsname *buf);
pid_t getppid(void);
pid_t vfork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
pid_t waitpid(pid_t pid, int *status, int options);
long ptrace(int req, pid_t pid, void *addr, void *data);
pid_t setsid(void);
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
int ioctl(int fd, unsigned long cmd, void *arg);

/* ── File I/O ────────────────────────────────────────────────────────── */

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int open(const char *path, int flags, int mode);
int close(int fd);
int pipe(int fds[2]);
int dup(int oldfd);
int dup2(int oldfd, int newfd);

/* ── Scatter/gather I/O ──────────────────────────────────────────────── */

ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
ssize_t readv(int fd, const struct iovec *iov, int iovcnt);

/* ── File system ─────────────────────────────────────────────────────── */

int lseek(int fd, int offset, int whence);
int getcwd(char *buf, size_t size);
int chdir(const char *path);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
int access(const char *path, int mode);
int mkdir(const char *path, int mode);
int unlink(const char *path);
int rmdir(const char *path);
int stat(const char *path, struct stat *buf);
int getdents(int fd, struct dirent *buf, size_t count);
int statfs64(const char *path, long sz, struct statfs *buf);

/* ── Memory management ──────────────────────────────────────────────── */

void *brk(void *addr);

/* ── Signals ───────────────────────────────────────────────────────── */

int kill(pid_t pid, int sig);
int sigaction(int sig, void *handler, void *old_handler);

/* Sigaction internals.  The POSIX-style sigaction() wrapper lives in
 * arch-specific user/sigaction.c; it builds a struct ppap_sigaction
 * with sa_restorer pointing at _ppap_sigreturn_trampoline (defined in
 * each arch's user/syscall.S) and forwards to rt_sigaction (also in
 * user/syscall.S). */
struct ppap_sigaction {
  void (*sa_handler)(int);
  unsigned long sa_flags;
  void (*sa_restorer)(void);
  unsigned long sa_mask[2];
};
void _ppap_sigreturn_trampoline(void);
int rt_sigaction(int sig, const struct ppap_sigaction *act,
                 struct ppap_sigaction *oact, int sigsetsize);

/* ── Sleep ─────────────────────────────────────────────────────────── */

int nanosleep(const void *req, void *rem);

/* ── Time ──────────────────────────────────────────────────────────── */

/* clock_gettime maps to SYS_CLOCK_GETTIME32 (0x0501).
 * tp must point to struct { long tv_sec; long tv_nsec; }. */
int clock_gettime(int clk_id, void *tp);

/* ── Poll ──────────────────────────────────────────────────────────── */

int ppoll(struct pollfd *fds, int nfds, void *timeout, void *sigmask,
          int sigsetsize);

/* ── System control ───────────────────────────────────────────────── */

void poweroff(void);

#endif /* __xtensa__ */

#endif /* PPAP_USER_SYSCALL_H */
