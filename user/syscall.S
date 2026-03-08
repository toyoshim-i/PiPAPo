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
@
@ PPAP syscall numbers: 16-bit, high byte = group, low byte = index.
@ Group 0 (0x00xx) fits in movs r7, #N.  Groups 1+ need ldr r7, =N.
@ =============================================================================

.syntax unified
.cpu cortex-m0plus
.thumb

@ --- _exit (SYS_EXIT = 0x0000) ------------------------------------------------
@ void _exit(int status);
.section .text._exit, "ax", %progbits
.global _exit
.thumb_func
_exit:
    movs r7, #0         @ SYS_EXIT
    svc  0
    b    .               @ should never return

@ --- read (SYS_READ = 0x0100) -------------------------------------------------
@ ssize_t read(int fd, void *buf, size_t n);
.section .text.read, "ax", %progbits
.global read
.thumb_func
read:
    push {r7, lr}
    ldr  r7, =0x0100    @ SYS_READ
    svc  0
    pop  {r7, pc}

@ --- write (SYS_WRITE = 0x0101) -----------------------------------------------
@ ssize_t write(int fd, const void *buf, size_t n);
.section .text.write, "ax", %progbits
.global write
.thumb_func
write:
    push {r7, lr}
    ldr  r7, =0x0101    @ SYS_WRITE
    svc  0
    pop  {r7, pc}

@ --- open (SYS_OPEN = 0x0102) -------------------------------------------------
@ int open(const char *path, int flags, int mode);
.section .text.open, "ax", %progbits
.global open
.thumb_func
open:
    push {r7, lr}
    ldr  r7, =0x0102    @ SYS_OPEN
    svc  0
    pop  {r7, pc}

@ --- close (SYS_CLOSE = 0x0103) -----------------------------------------------
@ int close(int fd);
.section .text.close, "ax", %progbits
.global close
.thumb_func
close:
    push {r7, lr}
    ldr  r7, =0x0103    @ SYS_CLOSE
    svc  0
    pop  {r7, pc}

@ --- waitpid (SYS_WAITPID = 0x0004) -------------------------------------------
@ pid_t waitpid(pid_t pid, int *status, int options);
.section .text.waitpid, "ax", %progbits
.global waitpid
.thumb_func
waitpid:
    push {r7, lr}
    movs r7, #4          @ SYS_WAITPID
    svc  0
    pop  {r7, pc}

@ --- execve (SYS_EXECVE = 0x0003) ---------------------------------------------
@ int execve(const char *path, char *const argv[], char *const envp[]);
.section .text.execve, "ax", %progbits
.global execve
.thumb_func
execve:
    push {r7, lr}
    movs r7, #3          @ SYS_EXECVE
    svc  0
    pop  {r7, pc}

@ --- setsid (SYS_SETSID = 0x000B) ---------------------------------------------
@ pid_t setsid(void);
.section .text.setsid, "ax", %progbits
.global setsid
.thumb_func
setsid:
    push {r7, lr}
    movs r7, #0x0B       @ SYS_SETSID
    svc  0
    pop  {r7, pc}

@ --- ioctl (SYS_IOCTL = 0x0107) -----------------------------------------------
@ int ioctl(int fd, unsigned long cmd, void *arg);
.section .text.ioctl, "ax", %progbits
.global ioctl
.thumb_func
ioctl:
    push {r7, lr}
    ldr  r7, =0x0107    @ SYS_IOCTL
    svc  0
    pop  {r7, pc}

@ --- getpid (SYS_GETPID = 0x0006) ---------------------------------------------
@ int getpid(void);
.section .text.getpid, "ax", %progbits
.global getpid
.thumb_func
getpid:
    push {r7, lr}
    movs r7, #6          @ SYS_GETPID
    svc  0
    pop  {r7, pc}

@ --- brk (SYS_BRK = 0x0400) ---------------------------------------------------
@ void *brk(void *addr);
.section .text.brk, "ax", %progbits
.global brk
.thumb_func
brk:
    push {r7, lr}
    ldr  r7, =0x0400    @ SYS_BRK
    svc  0
    pop  {r7, pc}

@ --- dup (SYS_DUP = 0x0104) ---------------------------------------------------
@ int dup(int oldfd);
.section .text.dup, "ax", %progbits
.global dup
.thumb_func
dup:
    push {r7, lr}
    ldr  r7, =0x0104    @ SYS_DUP
    svc  0
    pop  {r7, pc}

@ --- pipe (SYS_PIPE = 0x0106) -------------------------------------------------
@ int pipe(int fds[2]);
.section .text.pipe, "ax", %progbits
.global pipe
.thumb_func
pipe:
    push {r7, lr}
    ldr  r7, =0x0106    @ SYS_PIPE
    svc  0
    pop  {r7, pc}

@ --- dup2 (SYS_DUP2 = 0x0105) -------------------------------------------------
@ int dup2(int oldfd, int newfd);
.section .text.dup2, "ax", %progbits
.global dup2
.thumb_func
dup2:
    push {r7, lr}
    ldr  r7, =0x0105    @ SYS_DUP2
    svc  0
    pop  {r7, pc}

@ --- kill (SYS_KILL = 0x0600) -------------------------------------------------
@ int kill(pid_t pid, int sig);
.section .text.kill, "ax", %progbits
.global kill
.thumb_func
kill:
    push {r7, lr}
    ldr  r7, =0x0600    @ SYS_KILL
    svc  0
    pop  {r7, pc}

@ --- sigaction (SYS_SIGACTION = 0x0601) ----------------------------------------
@ int sigaction(int sig, void *handler, void *old_handler);
.section .text.sigaction, "ax", %progbits
.global sigaction
.thumb_func
sigaction:
    push {r7, lr}
    ldr  r7, =0x0601    @ SYS_SIGACTION
    svc  0
    pop  {r7, pc}

@ --- vfork (SYS_VFORK = 0x0002) -----------------------------------------------
@ pid_t vfork(void);
.section .text.vfork, "ax", %progbits
.global vfork
.thumb_func
vfork:
    push {r7, lr}
    movs r7, #2          @ SYS_VFORK
    svc  0
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
    svc  0
    pop  {r4, r7, pc}

    .ltorg                 @ literal pool

@ --- nanosleep (SYS_NANOSLEEP = 0x0500) ---------------------------------------
@ int nanosleep(const void *req, void *rem);
.section .text.nanosleep, "ax", %progbits
.global nanosleep
.thumb_func
nanosleep:
    push {r7, lr}
    ldr  r7, =0x0500    @ SYS_NANOSLEEP
    svc  0
    pop  {r7, pc}
