| =============================================================================
| switch.S — M68K Context Switch
| =============================================================================
|
| m68k_context_switch() is called from the timer ISR (before RTE) or from
| the TRAP #1 handler when m68k_switch_pending is set.  It saves the
| callee-saved registers of the current process, calls sched_next() to
| pick the next process, and restores the incoming process's context.
|
| Stack layout for a suspended process (low→high, SP = lowest address):
|
|   [SP+0 ]  d2         ← pcb_t.sp points here
|   [SP+4 ]  d3
|   [SP+8 ]  d4
|   [SP+12]  d5
|   [SP+16]  d6
|   [SP+20]  d7
|   [SP+24]  a2
|   [SP+28]  a3
|   [SP+32]  a4
|   [SP+36]  a5
|   [SP+40]  a6
|   [SP+44]  SR  (16-bit) ─┐  exception frame (pushed by hardware or
|   [SP+46]  PC  (32-bit)  ┘  constructed by proc_setup_stack)
|
| PCB_SP_OFFSET: byte offset of `sp` in pcb_t (= 44 on m68k).
| MUST match offsetof(pcb_t, sp); verified by _Static_assert in proc.c.
| =============================================================================

    .equ PCB_SP_OFFSET, 44       | offsetof(pcb_t, sp) — see proc.h

    .text
    .even

| -----------------------------------------------------------------------------
| m68k_context_switch — save current, pick next, restore next
|
| Called from timer ISR or TRAP #1 when m68k_switch_pending != 0.
| The caller's exception frame (SR + PC) is already on the supervisor stack.
| We save callee-saved registers on top of it, store SP in the PCB, call
| sched_next(), then restore the incoming process.
|
| On entry: interrupts are masked (called from ISR or TRAP handler).
| On exit:  returns via RTE to the incoming process.
| -----------------------------------------------------------------------------
    .globl m68k_context_switch
m68k_context_switch:
    | Clear the switch-pending flag
    clr.l   m68k_switch_pending

    | ── Save outgoing context ────────────────────────────────────────
    | Save callee-saved registers: d2-d7/a2-a6 (11 × 4 = 44 bytes)
    | The exception frame (SR + PC) was pushed by hardware before we got here.
    movem.l %d2-%d7/%a2-%a6,%sp@-

    | Store the adjusted SP into current->sp
    move.l  current_core,%a0       | a0 = current PCB pointer
    move.l  %sp,%a0@(PCB_SP_OFFSET)  | current->sp = SP

    | ── Call C scheduler ─────────────────────────────────────────────
    jsr     sched_next             | returns next pcb_t* in d0

    | ── Update current_core[0] ───────────────────────────────────────
    | m68k QEMU is single-core, so current_core[0] is always used.
    move.l  %d0,current_core       | current_core[0] = next

    | ── Restore incoming context ─────────────────────────────────────
    move.l  %d0,%a0
    move.l  %a0@(PCB_SP_OFFSET),%sp  | SP = next->sp

    | Restore callee-saved registers
    movem.l %sp@+,%d2-%d7/%a2-%a6

    | Return from exception — pops SR + PC from stack
    rte


| -----------------------------------------------------------------------------
| m68k_trap1_handler — TRAP #1 cooperative yield
|
| Called via TRAP #1 instruction from thread context.  Hardware pushes
| SR + PC onto the supervisor stack before entering this handler.
| We check m68k_switch_pending and perform a context switch if set.
| -----------------------------------------------------------------------------
    .globl m68k_trap1_handler
m68k_trap1_handler:
    | Set the switch-pending flag (TRAP #1 = explicit yield request)
    move.l  #1,m68k_switch_pending

    | Fall through to check-and-switch
    | (same as what the timer ISR does before RTE)
    tst.l   m68k_switch_pending
    beq.s   1f
    jmp     m68k_context_switch
1:  rte
