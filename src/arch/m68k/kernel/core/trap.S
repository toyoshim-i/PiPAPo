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
| the return value into regs[0] (the d0 slot).  It also handles syscall
| restart internally (looping on current->syscall_needs_restart) instead of
| using the global syscall_restart flag, which is unsafe on m68k because
| nested TRAP handlers share globals across processes.
|
| Post-dispatch check:
|   exec_pending — sys_execve loaded a new process image.
|   Do a full context restore from current->sp (new SW frame).
| =============================================================================

    .equ PCB_SP_OFFSET,  44      | offsetof(pcb_t, sp)  — see proc.h
    .equ PCB_USP_OFFSET, 48      | offsetof(pcb_t, usp) — see proc.h

    .text
    .even

    .globl m68k_trap0_handler
m68k_trap0_handler:
    | Save ALL registers (d0-d7/a0-a6 = 15 × 4 = 60 bytes)
    movem.l %d0-%d7/%a0-%a6,%sp@-

    | Save USP into current PCB so syscalls (e.g. vfork) see the
    | actual user stack pointer, not a stale value from the last switch.
    move.l  current_core,%a0
    move.l  %usp,%a1
    move.l  %a1,%a0@(PCB_USP_OFFSET)

    | Publish the trap frame SSP pointer for sys_rt_sigreturn to locate
    | the SR/PC slots it needs to overwrite when unwinding a signal.
    move.l  %sp,m68k_trap_frame_sp

    | Pass pointer to saved register frame as argument.
    | m68k C ABI: first arg on stack.
    | Avoid `move.l %sp,%sp@-` — 68000 stores the decremented value,
    | making the passed pointer 4 bytes too low.  Use a scratch register.
    move.l  %sp,%d0              | d0 = pointer to saved register frame
    move.l  %d0,%sp@-            | push as argument
    jsr     m68k_syscall_entry
    addq.l  #4,%sp               | clean up 1 arg

    | --- Check: exec_pending (sys_execve loaded new image) ---
    tst.l   exec_pending
    bne.s   .Lexec_restore

    | If a syscall requested scheduling (sched_switch), switch now
    | before returning to user mode.
    tst.l   switch_pending
    beq.s   .Lret_current
    clr.l   switch_pending

    | Save current SP/USP into PCB
    move.l  current_core,%a0
    move.l  %sp,%a0@(PCB_SP_OFFSET)
    move.l  %usp,%a1
    move.l  %a1,%a0@(PCB_USP_OFFSET)

    | Pick next runnable process and switch current_core
    jsr     sched_next
    move.l  %d0,current_core

    | Load next SP/USP
    move.l  %d0,%a0
    move.l  %a0@(PCB_SP_OFFSET),%sp
    move.l  %a0@(PCB_USP_OFFSET),%a1
    move.l  %a1,%usp

.Lret_current:
    | Return value is already in regs[0] (d0 slot on stack),
    | written by m68k_syscall_entry.

    | Restore all registers (d0 now contains return value)
    movem.l %sp@+,%d0-%d7/%a0-%a6

    | Return from exception — pops SR + PC
    rte

.Lexec_restore:
    | --- sys_execve: full context restore from new process image ---
    clr.l   exec_pending

    | Load current->sp (points to the new frame built by execve).
    | Switch stack to the new process's frame and restore from there.
    move.l  current_core,%a0
    move.l  %a0@(PCB_SP_OFFSET),%sp

    | Now on the NEW stack — safe to free the old stack page.
    jsr     exec_free_old_stack

    | Restore USP for the new process (set before movem/rte)
    move.l  current_core,%a0
    move.l  %a0@(PCB_USP_OFFSET),%a1
    move.l  %a1,%usp

    | execve can request an immediate stop/yield (e.g. ptrace exec-stop).
    tst.l   switch_pending
    beq.s   .Lexec_return
    clr.l   switch_pending

    move.l  current_core,%a0
    move.l  %sp,%a0@(PCB_SP_OFFSET)
    move.l  %usp,%a1
    move.l  %a1,%a0@(PCB_USP_OFFSET)

    jsr     sched_next
    move.l  %d0,current_core

    move.l  %d0,%a0
    move.l  %a0@(PCB_SP_OFFSET),%sp
    move.l  %a0@(PCB_USP_OFFSET),%a1
    move.l  %a1,%usp

.Lexec_return:
    | Restore all registers from the new frame and return
    movem.l %sp@+,%d0-%d7/%a0-%a6
    rte
