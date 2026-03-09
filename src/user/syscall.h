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
pid_t getppid(void);
pid_t vfork(void);
int   execve(const char *path, char *const argv[], char *const envp[]);
pid_t waitpid(pid_t pid, int *status, int options);
pid_t setsid(void);
int   setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
int   ioctl(int fd, unsigned long cmd, void *arg);

/* ── File I/O ────────────────────────────────────────────────────────────── */

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int     open(const char *path, int flags, int mode);
int     close(int fd);
int     pipe(int fds[2]);
int     dup(int oldfd);
int     dup2(int oldfd, int newfd);

/* ── File system ─────────────────────────────────────────────────────────── */

int     lseek(int fd, int offset, int whence);
int     getcwd(char *buf, size_t size);
int     chdir(const char *path);
int     access(const char *path, int mode);

/* lseek whence values */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* open flags */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2

/* access mode bits */
#define F_OK  0
#define R_OK  4

/* ── Memory management ──────────────────────────────────────────────────── */

void *brk(void *addr);

/* ── Signals ───────────────────────────────────────────────────────────── */

int kill(pid_t pid, int sig);
int sigaction(int sig, void *handler, void *old_handler);

/* ── Sleep ─────────────────────────────────────────────────────────────── */

int nanosleep(const void *req, void *rem);

/* ── Poll ──────────────────────────────────────────────────────────────── */

struct pollfd {
    int   fd;
    short events;
    short revents;
};

#define POLLIN    0x0001
#define POLLOUT   0x0004
#define POLLERR   0x0008
#define POLLHUP   0x0010
#define POLLNVAL  0x0020

int ppoll(struct pollfd *fds, int nfds, void *timeout,
          void *sigmask, int sigsetsize);

#endif /* PPAP_USER_SYSCALL_H */
