@ =============================================================================
@ syscall.S — SVC syscall stubs for PPAP user-space programs
@ =============================================================================
@
@ ARM EABI convention:
@   r7  = syscall number
@   r0–r3 = arguments
@   r0  = return value (negative errno on error)
@   svc 0 triggers the SVC exception
@
@ Each stub: push {r7,lr} → load r7 with syscall number → svc 0 → pop {r7,pc}
@ r7 is callee-saved in ARM AAPCS, so we must preserve it across the call.
@ =============================================================================

.syntax unified
.cpu cortex-m0plus
.thumb

@ --- _exit (SYS_EXIT = 1) ---------------------------------------------------
@ void _exit(int status);
@ Does not return.
.section .text._exit, "ax", %progbits
.global _exit
.thumb_func
_exit:
    movs r7, #1
    svc  0
    b    .             @ should never return

@ --- read (SYS_READ = 3) ----------------------------------------------------
@ ssize_t read(int fd, void *buf, size_t n);
.section .text.read, "ax", %progbits
.global read
.thumb_func
read:
    push {r7, lr}
    movs r7, #3
    svc  0
    pop  {r7, pc}

@ --- write (SYS_WRITE = 4) --------------------------------------------------
@ ssize_t write(int fd, const void *buf, size_t n);
.section .text.write, "ax", %progbits
.global write
.thumb_func
write:
    push {r7, lr}
    movs r7, #4
    svc  0
    pop  {r7, pc}

@ --- open (SYS_OPEN = 5) ----------------------------------------------------
@ int open(const char *path, int flags, int mode);
.section .text.open, "ax", %progbits
.global open
.thumb_func
open:
    push {r7, lr}
    movs r7, #5
    svc  0
    pop  {r7, pc}

@ --- close (SYS_CLOSE = 6) --------------------------------------------------
@ int close(int fd);
.section .text.close, "ax", %progbits
.global close
.thumb_func
close:
    push {r7, lr}
    movs r7, #6
    svc  0
    pop  {r7, pc}

@ --- waitpid (SYS_WAITPID = 7) ------------------------------------------------
@ pid_t waitpid(pid_t pid, int *status, int options);
.section .text.waitpid, "ax", %progbits
.global waitpid
.thumb_func
waitpid:
    push {r7, lr}
    movs r7, #7
    svc  0
    pop  {r7, pc}

@ --- execve (SYS_EXECVE = 11) ------------------------------------------------
@ int execve(const char *path, char *const argv[], char *const envp[]);
.section .text.execve, "ax", %progbits
.global execve
.thumb_func
execve:
    push {r7, lr}
    movs r7, #11
    svc  0
    pop  {r7, pc}

@ --- setsid (SYS_SETSID = 66) ------------------------------------------------
@ pid_t setsid(void);
.section .text.setsid, "ax", %progbits
.global setsid
.thumb_func
setsid:
    push {r7, lr}
    movs r7, #66
    svc  0
    pop  {r7, pc}

@ --- ioctl (SYS_IOCTL = 54) --------------------------------------------------
@ int ioctl(int fd, unsigned long cmd, void *arg);
.section .text.ioctl, "ax", %progbits
.global ioctl
.thumb_func
ioctl:
    push {r7, lr}
    movs r7, #54
    svc  0
    pop  {r7, pc}

@ --- getpid (SYS_GETPID = 20) -----------------------------------------------
@ int getpid(void);
.section .text.getpid, "ax", %progbits
.global getpid
.thumb_func
getpid:
    push {r7, lr}
    movs r7, #20
    svc  0
    pop  {r7, pc}

@ --- brk (SYS_BRK = 45) -------------------------------------------------------
@ void *brk(void *addr);
.section .text.brk, "ax", %progbits
.global brk
.thumb_func
brk:
    push {r7, lr}
    movs r7, #45
    svc  0
    pop  {r7, pc}

@ --- dup (SYS_DUP = 41) -------------------------------------------------------
@ int dup(int oldfd);
.section .text.dup, "ax", %progbits
.global dup
.thumb_func
dup:
    push {r7, lr}
    movs r7, #41
    svc  0
    pop  {r7, pc}

@ --- pipe (SYS_PIPE = 42) -----------------------------------------------------
@ int pipe(int fds[2]);
.section .text.pipe, "ax", %progbits
.global pipe
.thumb_func
pipe:
    push {r7, lr}
    movs r7, #42
    svc  0
    pop  {r7, pc}

@ --- dup2 (SYS_DUP2 = 63) -----------------------------------------------------
@ int dup2(int oldfd, int newfd);
.section .text.dup2, "ax", %progbits
.global dup2
.thumb_func
dup2:
    push {r7, lr}
    movs r7, #63
    svc  0
    pop  {r7, pc}

@ --- kill (SYS_KILL = 37) -----------------------------------------------------
@ int kill(pid_t pid, int sig);
.section .text.kill, "ax", %progbits
.global kill
.thumb_func
kill:
    push {r7, lr}
    movs r7, #37
    svc  0
    pop  {r7, pc}

@ --- sigaction (SYS_SIGACTION = 67) -------------------------------------------
@ int sigaction(int sig, void *handler, void *old_handler);
.section .text.sigaction, "ax", %progbits
.global sigaction
.thumb_func
sigaction:
    push {r7, lr}
    movs r7, #67
    svc  0
    pop  {r7, pc}

@ --- vfork (SYS_VFORK = 190) -------------------------------------------------
@ pid_t vfork(void);
.section .text.vfork, "ax", %progbits
.global vfork
.thumb_func
vfork:
    push {r7, lr}
    movs r7, #190
    svc  0
    pop  {r7, pc}

@ --- ppoll (SYS_PPOLL_TIME64 = 414) ------------------------------------------
@ int ppoll(struct pollfd *fds, int nfds, void *timeout, void *sigmask, int sigsetsize);
@ 5-arg syscall: arg5 (sigsetsize) goes in r4.
@ 414 > 255 so movs r7, #414 won't fit in Thumb-1; use ldr r7, =414 instead.
.section .text.ppoll, "ax", %progbits
.global ppoll
.thumb_func
ppoll:
    push {r4, r7, lr}
    ldr  r4, [sp, #12]    @ 5th arg from stack (above saved r4,r7,lr)
    ldr  r7, =414         @ SYS_PPOLL_TIME64
    svc  0
    pop  {r4, r7, pc}

    .ltorg                 @ literal pool for the ldr r7, =414

@ --- nanosleep (SYS_NANOSLEEP = 162) -----------------------------------------
@ int nanosleep(const void *req, void *rem);
.section .text.nanosleep, "ax", %progbits
.global nanosleep
.thumb_func
nanosleep:
    push {r7, lr}
    movs r7, #162
    svc  0
    pop  {r7, pc}
