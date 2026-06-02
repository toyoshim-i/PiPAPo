| =============================================================================
| switch.S — M68K Context Switch + Timer ISR
| =============================================================================
|
| Context switch saves ALL general-purpose registers (d0-d7/a0-a6) so the
| same stack frame format is used by both cooperative yield (TRAP #1) and
| preemptive timer interrupt.  This uniformity is essential: a process saved
| by the timer ISR may be resumed from the TRAP #1 path (or vice versa).
|
| Stack layout for a suspended process (low→high, SP = lowest address):
|
|   [SP+0 ]  d0         ← pcb_t.sp points here
|   [SP+4 ]  d1
|   [SP+8 ]  d2
|   [SP+12]  d3
|   [SP+16]  d4
|   [SP+20]  d5
|   [SP+24]  d6
|   [SP+28]  d7
|   [SP+32]  a0
|   [SP+36]  a1
|   [SP+40]  a2
|   [SP+44]  a3
|   [SP+48]  a4
|   [SP+52]  a5
|   [SP+56]  a6
|   [SP+60]  SR  (16-bit)  ─┐  exception frame (pushed by hardware or
|   [SP+62]  PC  (32-bit)   ┘  constructed by proc_setup_stack)
|
| PCB_SP_OFFSET: byte offset of `sp` in pcb_t (= 44 on m68k).
| MUST match offsetof(pcb_t, sp); verified by _Static_assert in proc.c.
| =============================================================================

    .equ PCB_SP_OFFSET,  44      | offsetof(pcb_t, sp)  — see proc.h
    .equ PCB_USP_OFFSET, 48      | offsetof(pcb_t, usp) — see proc.h
    .equ PCB_EXEC_RESTORE_PENDING_OFFSET, 1375

    .text
    .even

| -----------------------------------------------------------------------------
| m68k_ctx_switch — common SSP/USP handoff
|
| Enter with the full register frame already saved at SP.  The caller's return
| address is popped into a callee-saved address register before sched_next(),
| because the helper changes SP to the incoming process's supervisor stack and
| cannot return with RTS after that handoff.
| -----------------------------------------------------------------------------
    .globl m68k_ctx_switch
m68k_ctx_switch:
    move.l  %sp@+,%a2              | return address; SP again points at frame

    | Store SP (SSP) and USP into current PCB
    move.l  current_core,%a0
    move.l  %sp,%a0@(PCB_SP_OFFSET)
    move.l  %usp,%a1
    move.l  %a1,%a0@(PCB_USP_OFFSET)

    | Pick next process
    jsr     sched_next             | returns next pcb_t* in d0

    | Update current_core[0]
    move.l  %d0,current_core

    | Restore incoming context (SSP + USP)
    move.l  %d0,%a0
    move.l  %a0@(PCB_SP_OFFSET),%sp
    move.l  %a0@(PCB_USP_OFFSET),%a1
    move.l  %a1,%usp

    jmp     %a2@

| -----------------------------------------------------------------------------
| m68k_trap1_handler — TRAP #1 cooperative yield
|
| Called via TRAP #1 instruction from thread context.  Hardware pushes
| SR + PC onto the supervisor stack before entering this handler.
| Always performs a context switch (unconditional yield).
| -----------------------------------------------------------------------------
    .globl m68k_trap1_handler
m68k_trap1_handler:
    | Save ALL registers (d0-d7/a0-a6 = 15 × 4 = 60 bytes)
    movem.l %d0-%d7/%a0-%a6,%sp@-

    jsr     m68k_ctx_switch

    | Restore a blocked vfork parent's saved resume frame before user return.
    jsr     m68k_vfork_restore_frame

    | Restore all registers (a1 overwritten by movem from saved frame)
    movem.l %sp@+,%d0-%d7/%a0-%a6

    | Return from exception — pops SR + PC (SR restores user/supv mode)
    rte

| -----------------------------------------------------------------------------
| m68k_trace_handler — Trace exception (vector 9)
|
| Used for native single-step on m68k real-surface tracees.
| Saves full context, lets C trace code decide whether to stop the tracee,
| then performs a pending context switch if requested.
| -----------------------------------------------------------------------------
    .globl m68k_trace_handler
m68k_trace_handler:
    | Save ALL registers
    movem.l %d0-%d7/%a0-%a6,%sp@-

    | C helper: trace_m68k_trace_exception(regs)
    move.l  %sp,%d0
    move.l  %d0,%sp@-
    jsr     trace_m68k_trace_exception
    addq.l  #4,%sp

    | If tracing requested a stop, sched_switch() set switch_pending.
    tst.l   switch_pending
    beq.s   .Ltrace_no_switch
    clr.l   switch_pending

    jsr     m68k_ctx_switch

.Ltrace_no_switch:
    | Restore vfork parent slice on every user-mode rte path.
    jsr     m68k_vfork_restore_frame

    | Restore all registers and return from exception
    movem.l %sp@+,%d0-%d7/%a0-%a6
    rte


| -----------------------------------------------------------------------------
| m68k_timer_isr — Goldfish RTC timer interrupt (autovector level 6)
|
| Called when the Goldfish RTC alarm fires.  Hardware pushes SR + PC.
| We save all registers, call the C handler (which clears the interrupt,
| schedules the next alarm, and ticks the scheduler), then check if a
| context switch is pending.
| -----------------------------------------------------------------------------
    .globl m68k_timer_isr
m68k_timer_isr:
    | Save ALL registers
    movem.l %d0-%d7/%a0-%a6,%sp@-

    addq.w  #1,m68k_irq_depth

    | Call C handler: clear IRQ, set next alarm, sched_timer_tick()
    jsr     m68k_timer_handler_c

    | Check if scheduler requested a context switch
    tst.l   switch_pending
    beq.s   .Lno_switch

    | ── Context switch ────────────────────────────────────────────────────
    clr.l   switch_pending

    | exec_restore_pending: execve has set current->sp to the new process
    | frame and is about to return to trap.S .Lexec_restore.  Saving the
    | kernel SSP here would overwrite the new frame pointer and corrupt
    | exec.  Keep this state on the PCB so another process cannot clear it.
    move.l  current_core,%a0
    tst.b   %a0@(PCB_EXEC_RESTORE_PENDING_OFFSET)
    bne.s   .Lno_switch

    jsr     m68k_ctx_switch

.Lno_switch:
    subq.w  #1,m68k_irq_depth

    | Restore vfork parent slice on every user-mode rte path.
    jsr     m68k_vfork_restore_frame

    | Restore all registers
    movem.l %sp@+,%d0-%d7/%a0-%a6

    | Return from exception — pops SR + PC
    rte
