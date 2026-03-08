| =============================================================================
| trap.S — TRAP #0 Syscall Handler for M68K
| =============================================================================
|
| M68K syscall convention (matches Linux m68k / musl libc):
|   d0    = syscall number
|   d1-d5 = arguments 1-5
|   a0    = argument 6
|   d0    = return value (negative = -errno)
|
| When TRAP #0 fires:
|   - Hardware pushes {SR, PC} onto SSP (supervisor stack).
|   - CPU enters supervisor mode.
|   - All other registers are untouched.
|
| This handler saves all registers, calls m68k_syscall_entry() with a
| pointer to the saved register frame, and restores the modified frame
| (including the return value in d0) on exit.
|
| Register frame layout on stack (after movem.l save):
|   [SP+0 ]  d0  (syscall number → overwritten with return value)
|   [SP+4 ]  d1  (arg 1)
|   [SP+8 ]  d2  (arg 2)
|   [SP+12]  d3  (arg 3)
|   [SP+16]  d4  (arg 4)
|   [SP+20]  d5  (arg 5)
|   [SP+24]  d6
|   [SP+28]  d7
|   [SP+32]  a0  (arg 6)
|   [SP+36]  a1
|   [SP+40]  a2
|   [SP+44]  a3
|   [SP+48]  a4
|   [SP+52]  a5
|   [SP+56]  a6
|   [SP+60]  SR  (16-bit, exception frame)
|   [SP+62]  PC  (32-bit, exception frame)
|
| m68k_syscall_entry(regs) reads arguments from this frame and writes
| the return value into regs[0] (the d0 slot).
| =============================================================================

    .equ PCB_SP_OFFSET, 44       | offsetof(pcb_t, sp) — see proc.h

    .text
    .even

    .globl m68k_trap0_handler
m68k_trap0_handler:
    | Save ALL registers (d0-d7/a0-a6 = 15 × 4 = 60 bytes)
    movem.l %d0-%d7/%a0-%a6,%sp@-

    | Pass pointer to saved register frame as argument
    | m68k C ABI: first arg on stack
    move.l  %sp,%sp@-
    jsr     m68k_syscall_entry
    addq.l  #4,%sp               | clean up 1 arg

    | Return value is already in regs[0] (d0 slot on stack),
    | written by m68k_syscall_entry.

    | Restore all registers (d0 now contains return value)
    movem.l %sp@+,%d0-%d7/%a0-%a6

    | Return from exception — pops SR + PC
    rte
