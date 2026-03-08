| =============================================================================
| hello.S — Minimal m68k hello world for PPAP
| =============================================================================
|
| Uses TRAP #0 with Linux m68k syscall convention:
|   d0 = syscall number
|   d1 = arg1, d2 = arg2, d3 = arg3
|
| This is a static (non-PIC) binary with no data segment — text-only.
| The string lives in .rodata (part of the text PT_LOAD segment).
| =============================================================================

    .text
    .even
    .globl _start

_start:
    | write(1, msg, len) — SYS_WRITE = 4
    move.l  #4, %d0             | syscall number
    move.l  #1, %d1             | fd = stdout
    lea     msg(%pc), %a0
    move.l  %a0, %d2            | buf = &msg
    move.l  #(msg_end - msg), %d3  | count
    trap    #0

    | exit(0) — SYS_EXIT = 1
    move.l  #1, %d0
    move.l  #0, %d1
    trap    #0

    | should not reach here
.Lhang:
    bra.s   .Lhang

    .section .rodata
    .even
msg:
    .ascii  "Hello from m68k user space!\n"
msg_end:
