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
