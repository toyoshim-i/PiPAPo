/*
 * <unistd.h> — POSIX core syscalls.
 *
 * Declares the standard read / write / lseek / close / dup / pipe
 * family plus pid and process-group helpers.  Specialised entry
 * points (sigaction, ptrace, ioctl, mount, umount2, …) live under
 * their dedicated POSIX headers.
 *
 * Declarations only at this stage; the implementations are PPAP's
 * existing syscall stubs and will be wired up via this header in a
 * later step.
 */

#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int close(int fd);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int pipe(int fds[2]);
int lseek(int fd, int offset, int whence);

int chdir(const char *path);
int getcwd(char *buf, size_t size);
int access(const char *path, int mode);
int unlink(const char *path);
int rmdir(const char *path);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
int link(const char *oldpath, const char *newpath);

pid_t getpid(void);
pid_t getppid(void);
pid_t setsid(void);
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);

uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);

unsigned sleep(unsigned seconds);

pid_t fork(void);
pid_t vfork(void);
int execl(const char *path, const char *arg0, ...);
int execve(const char *path, char *const argv[], char *const envp[]);

int chmod(const char *path, int mode);
int ioctl(int fd, unsigned long cmd, void *arg);

char *getpass(const char *prompt);

void _exit(int status) __attribute__((noreturn));

#endif /* _UNISTD_H */
