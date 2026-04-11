@ =============================================================================
@ syscall.S — SVC syscall stubs for PPAP user-space programs
@ =============================================================================
@
@ ARM EABI convention:
@   r7  = syscall number
@   r0–r3 = arguments
@   r0  = return value (negative errno on error)
@
@ svc 0 triggers the SVC exception directly.
@
@ Each stub: push {r7,lr} → load r7 with syscall number → SYSCALL → pop {r7,pc}
@ r7 is callee-saved in ARM AAPCS, so we must preserve it across the call.
@
@ PPAP syscall numbers: 16-bit, high byte = group, low byte = index.
@ Group 0 (0x00xx) fits in movs r7, #N.  Groups 1+ need ldr r7, =N.
@ =============================================================================

.syntax unified
.cpu cortex-m0plus
.macro DO_SYSCALL
    svc  0
.endm
.thumb

@ --- _exit (SYS_EXIT = 0x0000) ------------------------------------------------
@ void _exit(int status);
.section .text._exit, "ax", %progbits
.global _exit
.thumb_func
_exit:
    movs r7, #0         @ SYS_EXIT
    DO_SYSCALL
    b    .               @ should never return

@ --- read (SYS_READ = 0x0100) -------------------------------------------------
@ ssize_t read(int fd, void *buf, size_t n);
.section .text.read, "ax", %progbits
.global read
.thumb_func
read:
    push {r7, lr}
    ldr  r7, =0x0100    @ SYS_READ
    DO_SYSCALL
    pop  {r7, pc}

@ --- write (SYS_WRITE = 0x0101) -----------------------------------------------
@ ssize_t write(int fd, const void *buf, size_t n);
.section .text.write, "ax", %progbits
.global write
.thumb_func
write:
    push {r7, lr}
    ldr  r7, =0x0101    @ SYS_WRITE
    DO_SYSCALL
    pop  {r7, pc}

@ --- open (SYS_OPEN = 0x0102) -------------------------------------------------
@ int open(const char *path, int flags, int mode);
.section .text.open, "ax", %progbits
.global open
.thumb_func
open:
    push {r7, lr}
    ldr  r7, =0x0102    @ SYS_OPEN
    DO_SYSCALL
    pop  {r7, pc}

@ --- close (SYS_CLOSE = 0x0103) -----------------------------------------------
@ int close(int fd);
.section .text.close, "ax", %progbits
.global close
.thumb_func
close:
    push {r7, lr}
    ldr  r7, =0x0103    @ SYS_CLOSE
    DO_SYSCALL
    pop  {r7, pc}

@ --- waitpid (SYS_WAITPID = 0x0004) -------------------------------------------
@ pid_t waitpid(pid_t pid, int *status, int options);
.section .text.waitpid, "ax", %progbits
.global waitpid
.thumb_func
waitpid:
    push {r7, lr}
    movs r7, #4          @ SYS_WAITPID
    DO_SYSCALL
    pop  {r7, pc}

@ --- execve (SYS_EXECVE = 0x0003) ---------------------------------------------
@ int execve(const char *path, char *const argv[], char *const envp[]);
.section .text.execve, "ax", %progbits
.global execve
.thumb_func
execve:
    push {r7, lr}
    movs r7, #3          @ SYS_EXECVE
    DO_SYSCALL
    pop  {r7, pc}

@ --- ptrace (SYS_PTRACE = 0x000F) ---------------------------------------------
@ long ptrace(int req, pid_t pid, void *addr, void *data);
.section .text.ptrace, "ax", %progbits
.global ptrace
.thumb_func
ptrace:
    push {r7, lr}
    movs r7, #0x0F
    DO_SYSCALL
    pop  {r7, pc}

@ --- setsid (SYS_SETSID = 0x000B) ---------------------------------------------
@ pid_t setsid(void);
.section .text.setsid, "ax", %progbits
.global setsid
.thumb_func
setsid:
    push {r7, lr}
    movs r7, #0x0B       @ SYS_SETSID
    DO_SYSCALL
    pop  {r7, pc}

@ --- ioctl (SYS_IOCTL = 0x0107) -----------------------------------------------
@ int ioctl(int fd, unsigned long cmd, void *arg);
.section .text.ioctl, "ax", %progbits
.global ioctl
.thumb_func
ioctl:
    push {r7, lr}
    ldr  r7, =0x0107    @ SYS_IOCTL
    DO_SYSCALL
    pop  {r7, pc}

@ --- getpid (SYS_GETPID = 0x0006) ---------------------------------------------
@ int getpid(void);
.section .text.getpid, "ax", %progbits
.global getpid
.thumb_func
getpid:
    push {r7, lr}
    movs r7, #6          @ SYS_GETPID
    DO_SYSCALL
    pop  {r7, pc}

@ --- brk (SYS_BRK = 0x0400) ---------------------------------------------------
@ void *brk(void *addr);
.section .text.brk, "ax", %progbits
.global brk
.thumb_func
brk:
    push {r7, lr}
    ldr  r7, =0x0400    @ SYS_BRK
    DO_SYSCALL
    pop  {r7, pc}

@ --- dup (SYS_DUP = 0x0104) ---------------------------------------------------
@ int dup(int oldfd);
.section .text.dup, "ax", %progbits
.global dup
.thumb_func
dup:
    push {r7, lr}
    ldr  r7, =0x0104    @ SYS_DUP
    DO_SYSCALL
    pop  {r7, pc}

@ --- pipe (SYS_PIPE = 0x0106) -------------------------------------------------
@ int pipe(int fds[2]);
.section .text.pipe, "ax", %progbits
.global pipe
.thumb_func
pipe:
    push {r7, lr}
    ldr  r7, =0x0106    @ SYS_PIPE
    DO_SYSCALL
    pop  {r7, pc}

@ --- dup2 (SYS_DUP2 = 0x0105) -------------------------------------------------
@ int dup2(int oldfd, int newfd);
.section .text.dup2, "ax", %progbits
.global dup2
.thumb_func
dup2:
    push {r7, lr}
    ldr  r7, =0x0105    @ SYS_DUP2
    DO_SYSCALL
    pop  {r7, pc}

@ --- kill (SYS_KILL = 0x0600) -------------------------------------------------
@ int kill(pid_t pid, int sig);
.section .text.kill, "ax", %progbits
.global kill
.thumb_func
kill:
    push {r7, lr}
    ldr  r7, =0x0600    @ SYS_KILL
    DO_SYSCALL
    pop  {r7, pc}

@ --- sigaction (SYS_SIGACTION = 0x0601) ----------------------------------------
@ int sigaction(int sig, void *handler, void *old_handler);
.section .text.sigaction, "ax", %progbits
.global sigaction
.thumb_func
sigaction:
    push {r7, lr}
    ldr  r7, =0x0601    @ SYS_SIGACTION
    DO_SYSCALL
    pop  {r7, pc}

@ --- vfork (SYS_VFORK = 0x0002) -----------------------------------------------
@ pid_t vfork(void);
.section .text.vfork, "ax", %progbits
.global vfork
.thumb_func
vfork:
    push {r7, lr}
    movs r7, #2          @ SYS_VFORK
    DO_SYSCALL
    pop  {r7, pc}

@ --- ppoll (SYS_PPOLL = 0x0701) -----------------------------------------------
@ int ppoll(struct pollfd *fds, int nfds, void *timeout, void *sigmask, int sigsetsize);
@ 5-arg syscall: arg5 (sigsetsize) goes in r4.
.section .text.ppoll, "ax", %progbits
.global ppoll
.thumb_func
ppoll:
    push {r4, r7, lr}
    ldr  r4, [sp, #12]    @ 5th arg from stack (above saved r4,r7,lr)
    ldr  r7, =0x0701      @ SYS_PPOLL
    DO_SYSCALL
    pop  {r4, r7, pc}

    .ltorg                 @ literal pool

@ --- getppid (SYS_GETPPID = 0x0008) -------------------------------------------
@ pid_t getppid(void);
.section .text.getppid, "ax", %progbits
.global getppid
.thumb_func
getppid:
    push {r7, lr}
    movs r7, #8          @ SYS_GETPPID
    DO_SYSCALL
    pop  {r7, pc}

@ --- setpgid (SYS_SETPGID = 0x0009) -------------------------------------------
@ int setpgid(pid_t pid, pid_t pgid);
.section .text.setpgid, "ax", %progbits
.global setpgid
.thumb_func
setpgid:
    push {r7, lr}
    movs r7, #9          @ SYS_SETPGID
    DO_SYSCALL
    pop  {r7, pc}

@ --- getpgid (SYS_GETPGID = 0x000A) -------------------------------------------
@ pid_t getpgid(pid_t pid);
.section .text.getpgid, "ax", %progbits
.global getpgid
.thumb_func
getpgid:
    push {r7, lr}
    movs r7, #0x0A       @ SYS_GETPGID
    DO_SYSCALL
    pop  {r7, pc}

@ --- lseek (SYS_LSEEK = 0x010B) -----------------------------------------------
@ int lseek(int fd, int offset, int whence);
.section .text.lseek, "ax", %progbits
.global lseek
.thumb_func
lseek:
    push {r7, lr}
    ldr  r7, =0x010B    @ SYS_LSEEK
    DO_SYSCALL
    pop  {r7, pc}

@ --- getcwd (SYS_GETCWD = 0x0203) ---------------------------------------------
@ int getcwd(char *buf, size_t size);
.section .text.getcwd, "ax", %progbits
.global getcwd
.thumb_func
getcwd:
    push {r7, lr}
    ldr  r7, =0x0203    @ SYS_GETCWD
    DO_SYSCALL
    pop  {r7, pc}

@ --- chdir (SYS_CHDIR = 0x0207) -----------------------------------------------
@ int chdir(const char *path);
.section .text.chdir, "ax", %progbits
.global chdir
.thumb_func
chdir:
    push {r7, lr}
    ldr  r7, =0x0207    @ SYS_CHDIR
    DO_SYSCALL
    pop  {r7, pc}

@ --- access (SYS_ACCESS = 0x0202) ---------------------------------------------
@ int access(const char *path, int mode);
.section .text.access, "ax", %progbits
.global access
.thumb_func
access:
    push {r7, lr}
    ldr  r7, =0x0202    @ SYS_ACCESS
    DO_SYSCALL
    pop  {r7, pc}

@ --- writev (SYS_WRITEV = 0x010A) ---------------------------------------------
@ ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
.section .text.writev, "ax", %progbits
.global writev
.thumb_func
writev:
    push {r7, lr}
    ldr  r7, =0x010A    @ SYS_WRITEV
    DO_SYSCALL
    pop  {r7, pc}

@ --- readv (SYS_READV = 0x0109) -----------------------------------------------
@ ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
.section .text.readv, "ax", %progbits
.global readv
.thumb_func
readv:
    push {r7, lr}
    ldr  r7, =0x0109    @ SYS_READV
    DO_SYSCALL
    pop  {r7, pc}

@ --- stat (SYS_STAT = 0x0200) -------------------------------------------------
@ int stat(const char *path, struct stat *buf);
.section .text.stat, "ax", %progbits
.global stat
.thumb_func
stat:
    push {r7, lr}
    ldr  r7, =0x0200    @ SYS_STAT
    DO_SYSCALL
    pop  {r7, pc}

@ --- getdents (SYS_GETDENTS = 0x0300) -----------------------------------------
@ int getdents(int fd, struct dirent *buf, size_t count);
.section .text.getdents, "ax", %progbits
.global getdents
.thumb_func
getdents:
    push {r7, lr}
    ldr  r7, =0x0300    @ SYS_GETDENTS
    DO_SYSCALL
    pop  {r7, pc}

@ --- statfs64 (SYS_STATFS64 = 0x0305) ----------------------------------------
@ int statfs64(const char *path, long sz, struct statfs *buf);
.section .text.statfs64, "ax", %progbits
.global statfs64
.thumb_func
statfs64:
    push {r7, lr}
    ldr  r7, =0x0305    @ SYS_STATFS64
    DO_SYSCALL
    pop  {r7, pc}

@ --- mkdir (SYS_MKDIR = 0x0204) -----------------------------------------------
@ int mkdir(const char *path, int mode);
.section .text.mkdir, "ax", %progbits
.global mkdir
.thumb_func
mkdir:
    push {r7, lr}
    ldr  r7, =0x0204    @ SYS_MKDIR
    DO_SYSCALL
    pop  {r7, pc}

@ --- unlink (SYS_UNLINK = 0x0206) ---------------------------------------------
@ int unlink(const char *path);
.section .text.unlink, "ax", %progbits
.global unlink
.thumb_func
unlink:
    push {r7, lr}
    ldr  r7, =0x0206    @ SYS_UNLINK
    DO_SYSCALL
    pop  {r7, pc}

@ --- rmdir (SYS_RMDIR = 0x0205) -----------------------------------------------
@ int rmdir(const char *path);
.section .text.rmdir, "ax", %progbits
.global rmdir
.thumb_func
rmdir:
    push {r7, lr}
    ldr  r7, =0x0205    @ SYS_RMDIR
    DO_SYSCALL
    pop  {r7, pc}

    .ltorg                 @ literal pool for new stubs

@ --- nanosleep (SYS_NANOSLEEP = 0x0500) ---------------------------------------
@ int nanosleep(const void *req, void *rem);
.section .text.nanosleep, "ax", %progbits
.global nanosleep
.thumb_func
nanosleep:
    push {r7, lr}
    ldr  r7, =0x0500    @ SYS_NANOSLEEP
    DO_SYSCALL
    pop  {r7, pc}

@ --- clock_gettime (SYS_CLOCK_GETTIME32 = 0x0501) ----------------------------
@ int clock_gettime(int clk_id, void *tp);
.section .text.clock_gettime, "ax", %progbits
.global clock_gettime
.thumb_func
clock_gettime:
    push {r7, lr}
    ldr  r7, =0x0501    @ SYS_CLOCK_GETTIME32
    DO_SYSCALL
    pop  {r7, pc}
