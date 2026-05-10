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
#include "common/mount.h"
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
int rename(const char *oldpath, const char *newpath);
int chmod(const char *path, int mode);
int link(const char *oldpath, const char *newpath);
int utimes(const char *path, const void *times);
int stat(const char *path, struct stat *buf);
int lstat(const char *path, struct stat *buf);
int getdents(int fd, struct dirent *buf, size_t count);
int statfs64(const char *path, long sz, struct statfs *buf);
int mount(const char *src, const char *tgt, const char *fstype, long flags,
          const void *data);
int umount2(const char *tgt, long flags);

/* ── Memory management ──────────────────────────────────────────────── */

void *brk(void *addr);

/* ── Signals ───────────────────────────────────────────────────────── */

int kill(pid_t pid, int sig);

/* signal() (POSIX) is declared in <signal.h>; it lives in the shared
 * src/user/lib/sigaction.c which builds a struct ppap_sigaction with
 * sa_restorer pointing at _ppap_sigreturn_trampoline (defined in each
 * arch's user/syscall.S) and forwards to rt_sigaction (also in
 * user/syscall.S).  i16 supplies its own asm signal() in
 * src/arch/i16/user/syscall.S instead. */
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

/* settimeofday maps to SYS_SETTIMEOFDAY (0x0506).
 * tv must point to struct timeval { long tv_sec; long tv_usec; }.
 * tz is accepted for POSIX compat and ignored by the kernel. */
int settimeofday(const void *tv, const void *tz);

/* ── Poll ──────────────────────────────────────────────────────────── */

int ppoll(struct pollfd *fds, int nfds, void *timeout, void *sigmask,
          int sigsetsize);

/* ── System control ───────────────────────────────────────────────── */

void poweroff(void);

#endif /* PPAP_USER_SYSCALL_H */
