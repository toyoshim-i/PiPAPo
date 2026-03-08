| =============================================================================
| syscall.S — TRAP #0 syscall stubs for PPAP m68k user-space programs
| =============================================================================
|
| Linux m68k syscall convention:
|   d0    = syscall number
|   d1-d5 = arguments 1-5
|   a0    = argument 6
|   d0    = return value (negative errno on error)
|   trap #0 triggers the syscall
|
| m68k C ABI: arguments on stack, return value in d0.
| d2-d7/a2-a6 are callee-saved; d0-d1/a0-a1 are caller-saved.
| For syscalls that use d2+ as arguments, we must save/restore
| callee-saved registers around the trap.
| =============================================================================

    .text
    .even

| --- _exit (SYS_EXIT = 1) ---------------------------------------------------
| void _exit(int status);
    .globl __exit
    .globl _exit
__exit:
_exit:
    move.l  4(%sp), %d1         | status
    moveq   #1, %d0             | SYS_EXIT
    trap    #0
    bra.s   _exit               | should never return

| --- read (SYS_READ = 3) ----------------------------------------------------
| ssize_t read(int fd, void *buf, size_t n);
    .globl read
read:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | fd
    move.l  12(%sp), %d2        | buf
    move.l  16(%sp), %d3        | n
    moveq   #3, %d0
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- write (SYS_WRITE = 4) --------------------------------------------------
| ssize_t write(int fd, const void *buf, size_t n);
    .globl write
write:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | fd
    move.l  12(%sp), %d2        | buf
    move.l  16(%sp), %d3        | n
    moveq   #4, %d0
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- open (SYS_OPEN = 5) ----------------------------------------------------
| int open(const char *path, int flags, int mode);
    .globl open
open:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | path
    move.l  12(%sp), %d2        | flags
    move.l  16(%sp), %d3        | mode
    moveq   #5, %d0
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- close (SYS_CLOSE = 6) --------------------------------------------------
| int close(int fd);
    .globl close
close:
    move.l  4(%sp), %d1         | fd
    moveq   #6, %d0
    trap    #0
    rts

| --- waitpid (SYS_WAITPID = 7) ----------------------------------------------
| pid_t waitpid(pid_t pid, int *status, int options);
    .globl waitpid
waitpid:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | pid
    move.l  12(%sp), %d2        | status
    move.l  16(%sp), %d3        | options
    moveq   #7, %d0
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- execve (SYS_EXECVE = 11) -----------------------------------------------
| int execve(const char *path, char *const argv[], char *const envp[]);
    .globl execve
execve:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | path
    move.l  12(%sp), %d2        | argv
    move.l  16(%sp), %d3        | envp
    moveq   #11, %d0
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- getpid (SYS_GETPID = 20) -----------------------------------------------
| int getpid(void);
    .globl getpid
getpid:
    moveq   #20, %d0
    trap    #0
    rts

| --- kill (SYS_KILL = 37) ---------------------------------------------------
| int kill(pid_t pid, int sig);
    .globl kill
kill:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | pid
    move.l  12(%sp), %d2        | sig
    moveq   #37, %d0
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- dup (SYS_DUP = 41) -----------------------------------------------------
| int dup(int oldfd);
    .globl dup
dup:
    move.l  4(%sp), %d1         | oldfd
    moveq   #41, %d0
    trap    #0
    rts

| --- pipe (SYS_PIPE = 42) ---------------------------------------------------
| int pipe(int fds[2]);
    .globl pipe
pipe:
    move.l  4(%sp), %d1         | fds
    moveq   #42, %d0
    trap    #0
    rts

| --- brk (SYS_BRK = 45) -----------------------------------------------------
| void *brk(void *addr);
    .globl brk
brk:
    move.l  4(%sp), %d1         | addr
    moveq   #45, %d0
    trap    #0
    rts

| --- ioctl (SYS_IOCTL = 54) -------------------------------------------------
| int ioctl(int fd, unsigned long cmd, void *arg);
    .globl ioctl
ioctl:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | fd
    move.l  12(%sp), %d2        | cmd
    move.l  16(%sp), %d3        | arg
    moveq   #54, %d0
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- dup2 (SYS_DUP2 = 63) ---------------------------------------------------
| int dup2(int oldfd, int newfd);
    .globl dup2
dup2:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | oldfd
    move.l  12(%sp), %d2        | newfd
    moveq   #63, %d0
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- setsid (SYS_SETSID = 66) -----------------------------------------------
| pid_t setsid(void);
    .globl setsid
setsid:
    moveq   #66, %d0
    trap    #0
    rts

| --- sigaction (SYS_SIGACTION = 67) ------------------------------------------
| int sigaction(int sig, void *handler, void *old_handler);
    .globl sigaction
sigaction:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | sig
    move.l  12(%sp), %d2        | handler
    move.l  16(%sp), %d3        | old_handler
    moveq   #67, %d0
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- nanosleep (SYS_NANOSLEEP = 162) ----------------------------------------
| int nanosleep(const void *req, void *rem);
    .globl nanosleep
nanosleep:
    move.l  %d2, %sp@-
    move.l  8(%sp), %d1         | req
    move.l  12(%sp), %d2        | rem
    move.l  #162, %d0
    trap    #0
    move.l  %sp@+, %d2
    rts

| --- vfork (SYS_VFORK = 190) ------------------------------------------------
| pid_t vfork(void);
    .globl vfork
vfork:
    move.l  #190, %d0
    trap    #0
    rts

| --- ppoll (SYS_PPOLL_TIME64 = 414) -----------------------------------------
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
    move.l  #414, %d0           | SYS_PPOLL_TIME64
    trap    #0
    movem.l %sp@+, %d2-%d5
    rts
