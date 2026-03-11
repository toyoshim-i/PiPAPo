| =============================================================================
| syscall.S — TRAP #0 syscall stubs for PPAP m68k user-space programs
| =============================================================================
|
| PPAP m68k syscall convention:
|   d0    = syscall number (16-bit: high byte = group, low byte = index)
|   d1-d5 = arguments 1-5
|   a0    = argument 6
|   d0    = return value (negative errno on error)
|   trap #0 triggers the syscall
|
| m68k C ABI: arguments on stack, return value in d0.
| d2-d7/a2-a6 are callee-saved; d0-d1/a0-a1 are caller-saved.
| For syscalls that use d2+ as arguments, we must save/restore
| ALL callee-saved registers we modify around the trap.
|
| Group 0 (0x00xx) fits in moveq.  Groups 1+ need move.l #N, %d0.
| =============================================================================

    .text
    .even

| --- _exit (SYS_EXIT = 0x0000) ------------------------------------------------
| void _exit(int status);
    .globl __exit
    .globl _exit
__exit:
_exit:
    move.l  4(%sp), %d1         | status
    moveq   #0, %d0             | SYS_EXIT
    trap    #0
    bra.s   _exit               | should never return

| --- read (SYS_READ = 0x0100) -------------------------------------------------
| ssize_t read(int fd, void *buf, size_t n);
    .globl read
read:
    movem.l %d2-%d3, %sp@-      | save d2-d3 (8 bytes)
    move.l  12(%sp), %d1        | fd
    move.l  16(%sp), %d2        | buf
    move.l  20(%sp), %d3        | n
    move.l  #0x0100, %d0
    trap    #0
    movem.l %sp@+, %d2-%d3
    rts

| --- write (SYS_WRITE = 0x0101) -----------------------------------------------
| ssize_t write(int fd, const void *buf, size_t n);
    .globl write
write:
    movem.l %d2-%d3, %sp@-      | save d2-d3 (8 bytes)
    move.l  12(%sp), %d1        | fd
    move.l  16(%sp), %d2        | buf
    move.l  20(%sp), %d3        | n
    move.l  #0x0101, %d0
    trap    #0
    movem.l %sp@+, %d2-%d3
    rts

| --- open (SYS_OPEN = 0x0102) -------------------------------------------------
| int open(const char *path, int flags, int mode);
    .globl open
open:
    movem.l %d2-%d3, %sp@-      | save d2-d3 (8 bytes)
    move.l  12(%sp), %d1        | path
    move.l  16(%sp), %d2        | flags
    move.l  20(%sp), %d3        | mode
    move.l  #0x0102, %d0
    trap    #0
    movem.l %sp@+, %d2-%d3
    rts

| --- close (SYS_CLOSE = 0x0103) -----------------------------------------------
| int close(int fd);
    .globl close
close:
    move.l  4(%sp), %d1         | fd
    move.l  #0x0103, %d0
    trap    #0
    rts

| --- waitpid (SYS_WAITPID = 0x0004) -------------------------------------------
| pid_t waitpid(pid_t pid, int *status, int options);
    .globl waitpid
waitpid:
    movem.l %d2-%d3, %sp@-      | save d2-d3 (8 bytes)
    move.l  12(%sp), %d1        | pid
    move.l  16(%sp), %d2        | status
    move.l  20(%sp), %d3        | options
    moveq   #4, %d0             | SYS_WAITPID
    trap    #0
    movem.l %sp@+, %d2-%d3
    rts

| --- execve (SYS_EXECVE = 0x0003) ---------------------------------------------
| int execve(const char *path, char *const argv[], char *const envp[]);
    .globl execve
execve:
    movem.l %d2-%d3, %sp@-      | save d2-d3 (8 bytes)
    move.l  12(%sp), %d1        | path
    move.l  16(%sp), %d2        | argv
    move.l  20(%sp), %d3        | envp
    moveq   #3, %d0             | SYS_EXECVE
    trap    #0
    movem.l %sp@+, %d2-%d3
    rts

| --- ptrace (SYS_PTRACE = 0x000F) ---------------------------------------------
| long ptrace(int req, pid_t pid, void *addr, void *data);
    .globl ptrace
ptrace:
    movem.l %d2-%d4, %sp@-
    move.l  16(%sp), %d1        | req
    move.l  20(%sp), %d2        | pid
    move.l  24(%sp), %d3        | addr
    move.l  28(%sp), %d4        | data
    moveq   #0x0F, %d0
    trap    #0
    movem.l %sp@+, %d2-%d4
    rts

| --- getpid (SYS_GETPID = 0x0006) ---------------------------------------------
| int getpid(void);
    .globl getpid
getpid:
    moveq   #6, %d0             | SYS_GETPID
    trap    #0
    rts

| --- kill (SYS_KILL = 0x0600) -------------------------------------------------
| int kill(pid_t pid, int sig);
    .globl kill
kill:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | pid
    move.l  12(%sp), %d2        | sig
    move.l  #0x0600, %d0
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- dup (SYS_DUP = 0x0104) ---------------------------------------------------
| int dup(int oldfd);
    .globl dup
dup:
    move.l  4(%sp), %d1         | oldfd
    move.l  #0x0104, %d0
    trap    #0
    rts

| --- pipe (SYS_PIPE = 0x0106) -------------------------------------------------
| int pipe(int fds[2]);
    .globl pipe
pipe:
    move.l  4(%sp), %d1         | fds
    move.l  #0x0106, %d0
    trap    #0
    rts

| --- brk (SYS_BRK = 0x0400) ---------------------------------------------------
| void *brk(void *addr);
    .globl brk
brk:
    move.l  4(%sp), %d1         | addr
    move.l  #0x0400, %d0
    trap    #0
    rts

| --- ioctl (SYS_IOCTL = 0x0107) -----------------------------------------------
| int ioctl(int fd, unsigned long cmd, void *arg);
    .globl ioctl
ioctl:
    movem.l %d2-%d3, %sp@-      | save d2-d3 (8 bytes)
    move.l  12(%sp), %d1        | fd
    move.l  16(%sp), %d2        | cmd
    move.l  20(%sp), %d3        | arg
    move.l  #0x0107, %d0
    trap    #0
    movem.l %sp@+, %d2-%d3
    rts

| --- dup2 (SYS_DUP2 = 0x0105) -------------------------------------------------
| int dup2(int oldfd, int newfd);
    .globl dup2
dup2:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | oldfd
    move.l  12(%sp), %d2        | newfd
    move.l  #0x0105, %d0
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- setsid (SYS_SETSID = 0x000B) ---------------------------------------------
| pid_t setsid(void);
    .globl setsid
setsid:
    moveq   #0x0B, %d0          | SYS_SETSID
    trap    #0
    rts

| --- sigaction (SYS_SIGACTION = 0x0601) ----------------------------------------
| int sigaction(int sig, void *handler, void *old_handler);
    .globl sigaction
sigaction:
    movem.l %d2-%d3, %sp@-      | save d2-d3 (8 bytes)
    move.l  12(%sp), %d1        | sig
    move.l  16(%sp), %d2        | handler
    move.l  20(%sp), %d3        | old_handler
    move.l  #0x0601, %d0
    trap    #0
    movem.l %sp@+, %d2-%d3
    rts

| --- getppid (SYS_GETPPID = 0x0008) ---------------------------------------------
| pid_t getppid(void);
    .globl getppid
getppid:
    moveq   #8, %d0             | SYS_GETPPID
    trap    #0
    rts

| --- setpgid (SYS_SETPGID = 0x0009) -------------------------------------------
| int setpgid(pid_t pid, pid_t pgid);
    .globl setpgid
setpgid:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | pid
    move.l  12(%sp), %d2        | pgid
    moveq   #9, %d0             | SYS_SETPGID
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- getpgid (SYS_GETPGID = 0x000A) -------------------------------------------
| pid_t getpgid(pid_t pid);
    .globl getpgid
getpgid:
    move.l  4(%sp), %d1         | pid
    moveq   #0x0A, %d0          | SYS_GETPGID
    trap    #0
    rts

| --- lseek (SYS_LSEEK = 0x010B) -----------------------------------------------
| int lseek(int fd, int offset, int whence);
    .globl lseek
lseek:
    movem.l %d2-%d3, %sp@-      | save d2-d3 (8 bytes)
    move.l  12(%sp), %d1        | fd
    move.l  16(%sp), %d2        | offset
    move.l  20(%sp), %d3        | whence
    move.l  #0x010B, %d0
    trap    #0
    movem.l %sp@+, %d2-%d3
    rts

| --- getcwd (SYS_GETCWD = 0x0203) ---------------------------------------------
| int getcwd(char *buf, size_t size);
    .globl getcwd
getcwd:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | buf
    move.l  12(%sp), %d2        | size
    move.l  #0x0203, %d0
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- chdir (SYS_CHDIR = 0x0207) -----------------------------------------------
| int chdir(const char *path);
    .globl chdir
chdir:
    move.l  4(%sp), %d1         | path
    move.l  #0x0207, %d0
    trap    #0
    rts

| --- access (SYS_ACCESS = 0x0202) ---------------------------------------------
| int access(const char *path, int mode);
    .globl access
access:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | path
    move.l  12(%sp), %d2        | mode
    move.l  #0x0202, %d0
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- writev (SYS_WRITEV = 0x010A) -----------------------------------------------
| ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
    .globl writev
writev:
    movem.l %d2-%d3, %sp@-      | save d2-d3 (8 bytes)
    move.l  12(%sp), %d1        | fd
    move.l  16(%sp), %d2        | iov
    move.l  20(%sp), %d3        | iovcnt
    move.l  #0x010A, %d0
    trap    #0
    movem.l %sp@+, %d2-%d3
    rts

| --- readv (SYS_READV = 0x0109) -----------------------------------------------
| ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
    .globl readv
readv:
    movem.l %d2-%d3, %sp@-      | save d2-d3 (8 bytes)
    move.l  12(%sp), %d1        | fd
    move.l  16(%sp), %d2        | iov
    move.l  20(%sp), %d3        | iovcnt
    move.l  #0x0109, %d0
    trap    #0
    movem.l %sp@+, %d2-%d3
    rts

| --- stat (SYS_STAT = 0x0200) -------------------------------------------------
| int stat(const char *path, struct stat *buf);
    .globl stat
stat:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | path
    move.l  12(%sp), %d2        | buf
    move.l  #0x0200, %d0
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- getdents (SYS_GETDENTS = 0x0300) -----------------------------------------
| int getdents(int fd, struct dirent *buf, size_t count);
    .globl getdents
getdents:
    movem.l %d2-%d3, %sp@-      | save d2-d3 (8 bytes)
    move.l  12(%sp), %d1        | fd
    move.l  16(%sp), %d2        | buf
    move.l  20(%sp), %d3        | count
    move.l  #0x0300, %d0
    trap    #0
    movem.l %sp@+, %d2-%d3
    rts

| --- mkdir (SYS_MKDIR = 0x0204) -----------------------------------------------
| int mkdir(const char *path, int mode);
    .globl mkdir
mkdir:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | path
    move.l  12(%sp), %d2        | mode
    move.l  #0x0204, %d0
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- unlink (SYS_UNLINK = 0x0206) ---------------------------------------------
| int unlink(const char *path);
    .globl unlink
unlink:
    move.l  4(%sp), %d1         | path
    move.l  #0x0206, %d0
    trap    #0
    rts

| --- rmdir (SYS_RMDIR = 0x0205) -----------------------------------------------
| int rmdir(const char *path);
    .globl rmdir
rmdir:
    move.l  4(%sp), %d1         | path
    move.l  #0x0205, %d0
    trap    #0
    rts

| --- nanosleep (SYS_NANOSLEEP = 0x0500) ----------------------------------------
| int nanosleep(const void *req, void *rem);
    .globl nanosleep
nanosleep:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | req
    move.l  12(%sp), %d2        | rem
    move.l  #0x0500, %d0
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- vfork (SYS_VFORK = 0x0002) -----------------------------------------------
| pid_t vfork(void);
    .globl vfork
vfork:
    moveq   #2, %d0             | SYS_VFORK
    trap    #0
    rts

| --- ppoll (SYS_PPOLL = 0x0701) -----------------------------------------------
| int ppoll(struct pollfd *fds, int nfds, void *timeout,
|           void *sigmask, int sigsetsize);
    .globl ppoll
ppoll:
    movem.l %d2-%d5, %sp@-      | save d2-d5 (16 bytes)
    move.l  20(%sp), %d1        | fds
    move.l  24(%sp), %d2        | nfds
    move.l  28(%sp), %d3        | timeout
    move.l  32(%sp), %d4        | sigmask
    move.l  36(%sp), %d5        | sigsetsize
    move.l  #0x0701, %d0        | SYS_PPOLL
    trap    #0
    movem.l %sp@+, %d2-%d5
    rts
