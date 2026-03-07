@ =============================================================================
@ getty.S — Minimal getty for PPAP (pure assembly, no GOT/data segment)
@
@ Combined getty + login for a single-user embedded system.
@ Called from inittab:
@   ttyS0::respawn:/bin/getty ttyS0
@   tty1::respawn:/bin/getty tty1
@
@ Flow:
@   1. setsid()         — become session leader
@   2. open /dev/<tty>  — as stdin, dup to stdout/stderr
@   3. TIOCSCTTY        — acquire controlling terminal
@   4. Print "Press Enter to activate this console."
@   5. Wait for Enter
@   6. execve /bin/sh
@
@ No .data/.got/.bss → kernel allocates only 1 page (stack).
@
@ All strings are in .text (same section as code) and accessed via ADR
@ (PC-relative), which gives the correct runtime flash address on XIP.
@ Using ldr =label would give link-time vaddrs (wrong without GOT relocation).
@
@ See getty.c for a portable C reference implementation (requires PIC,
@ uses 2 pages due to GOT).
@ =============================================================================

.syntax unified
.cpu cortex-m0plus
.thumb

@ ── Syscall numbers (Linux ARM EABI) ─────────────────────────────────────────
.equ SYS_EXIT,    1
.equ SYS_READ,    3
.equ SYS_WRITE,   4
.equ SYS_OPEN,    5
.equ SYS_CLOSE,   6
.equ SYS_EXECVE, 11
.equ SYS_SETSID, 66
.equ SYS_DUP2,   63
.equ SYS_IOCTL,  54

.equ O_RDWR,      2
.equ TIOCSCTTY,   0x540E

@ ── Entry point ──────────────────────────────────────────────────────────────
.section .text.crt0, "ax", %progbits
.align 2
.global _start
.thumb_func
_start:
    @ r4 = argv[1] (tty name), or default "ttyS0"
    ldr  r0, [sp]          @ argc
    cmp  r0, #2
    blt  .Luse_default
    ldr  r4, [sp, #8]      @ argv[1] — kernel copies argv strings to stack
    b    .Lgot_tty
.Luse_default:
    adr  r4, str_default_tty
.Lgot_tty:

    @ 1. setsid()
    movs r7, #SYS_SETSID
    svc  0

    @ 2. Build "/dev/<tty>" on the stack
    @    Reserve 32 bytes on stack for path buffer
    sub  sp, #32
    mov  r0, sp
    adr  r1, str_dev_prefix  @ "/dev/" — PC-relative, correct on XIP
    bl   .Lstr_copy            @ r0 = past "/dev/"
    mov  r1, r4                @ tty name (from argv or default)
    bl   .Lstr_copy            @ r0 = past tty name
    movs r1, #0
    strb r1, [r0]              @ NUL terminate

    @ close(0), close(1), close(2)
    movs r7, #SYS_CLOSE
    movs r0, #0
    svc  0
    movs r0, #1
    svc  0
    movs r0, #2
    svc  0

    @ fd = open(path, O_RDWR, 0)
    mov  r0, sp
    movs r1, #O_RDWR
    movs r2, #0
    movs r7, #SYS_OPEN
    svc  0

    @ If open failed, try argv[1] as-is (might be a full path)
    cmp  r0, #0
    bge  .Lopen_ok
    mov  r0, r4
    movs r1, #O_RDWR
    movs r2, #0
    movs r7, #SYS_OPEN
    svc  0
    cmp  r0, #0
    bge  .Lopen_ok

    @ Both opens failed — exit
    movs r0, #1
    movs r7, #SYS_EXIT
    svc  0

.Lopen_ok:
    @ r5 = fd
    mov  r5, r0

    @ Free the stack path buffer
    add  sp, #32

    @ If fd != 0, dup2(fd, 0) then close(fd)
    cmp  r5, #0
    beq  .Lfd_is_zero
    mov  r0, r5
    movs r1, #0
    movs r7, #SYS_DUP2
    svc  0
    mov  r0, r5
    movs r7, #SYS_CLOSE
    svc  0
.Lfd_is_zero:

    @ dup2(0, 1)
    movs r0, #0
    movs r1, #1
    movs r7, #SYS_DUP2
    svc  0

    @ dup2(0, 2)
    movs r0, #0
    movs r1, #2
    movs r7, #SYS_DUP2
    svc  0

    @ 3. ioctl(0, TIOCSCTTY, 1)
    movs r0, #0
    ldr  r1, .Ltiocsctty     @ constant value, not an address — literal pool OK
    movs r2, #1
    movs r7, #SYS_IOCTL
    svc  0

    @ 4. Write prompt
    movs r0, #1               @ fd = stdout
    adr  r1, str_prompt        @ PC-relative address
    movs r2, #str_prompt_len
    movs r7, #SYS_WRITE
    svc  0

    @ 5. Wait for Enter
.Lread_loop:
    sub  sp, #4
    movs r0, #0               @ fd = stdin
    mov  r1, sp               @ buf on stack
    movs r2, #1               @ count = 1
    movs r7, #SYS_READ
    svc  0
    cmp  r0, #0
    ble  .Lexit_fail           @ read error or EOF
    mov  r0, sp
    ldrb r0, [r0]
    add  sp, #4
    cmp  r0, #'\n'
    beq  .Lgot_enter
    cmp  r0, #'\r'
    beq  .Lgot_enter
    b    .Lread_loop

.Lgot_enter:
    @ Write newline
    movs r0, #1
    adr  r1, str_nl
    movs r2, #1
    movs r7, #SYS_WRITE
    svc  0

    @ 6. execve("/bin/sh", {"-sh", NULL}, {NULL})
    @    Build argv and envp on the stack:
    @      [sp+0]  = argv[0] = &str_dash_sh
    @      [sp+4]  = argv[1] = NULL
    @      [sp+8]  = envp[0] = NULL
    sub  sp, #12
    adr  r0, str_dash_sh
    str  r0, [sp]
    movs r0, #0
    str  r0, [sp, #4]
    str  r0, [sp, #8]
    adr  r0, str_bin_sh
    mov  r1, sp                @ argv
    mov  r2, sp
    adds r2, #8                @ envp
    movs r7, #SYS_EXECVE
    svc  0

    @ execve failed — write error and exit
    movs r0, #2               @ fd = stderr
    adr  r1, str_exec_fail
    movs r2, #str_exec_fail_len
    movs r7, #SYS_WRITE
    svc  0

.Lexit_fail:
    movs r0, #1
    movs r7, #SYS_EXIT
    svc  0
.Lhang:
    b    .Lhang

@ ── Helper: copy string r1→r0, return r0 past end ───────────────────────────
.align 2
.thumb_func
.Lstr_copy:
    ldrb r2, [r1]
    cmp  r2, #0
    beq  .Lstr_done
    strb r2, [r0]
    adds r0, #1
    adds r1, #1
    b    .Lstr_copy
.Lstr_done:
    bx   lr

@ ── Literal pool for non-address constants ───────────────────────────────────
.align 2
.Ltiocsctty:
    .word TIOCSCTTY

@ ── String constants (in .text for PC-relative ADR access on XIP) ────────────
@ ADR requires word-aligned targets within +1020 bytes forward.
.align 2
str_default_tty:
    .asciz "ttyS0"
.align 2
str_dev_prefix:
    .asciz "/dev/"
.align 2
str_prompt:
    .ascii "\nPress Enter to activate this console. "
    .equ str_prompt_len, . - str_prompt
.align 2
str_nl:
    .ascii "\n"
.align 2
str_dash_sh:
    .asciz "-sh"
.align 2
str_bin_sh:
    .asciz "/bin/sh"
.align 2
str_exec_fail:
    .ascii "getty: exec /bin/sh failed\n"
    .equ str_exec_fail_len, . - str_exec_fail
