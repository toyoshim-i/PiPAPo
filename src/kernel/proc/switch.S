@ =============================================================================
@ switch.S — Cortex-M0+ Context Switch (PendSV_Handler)
@ =============================================================================
@
@ PendSV is triggered by SysTick_Handler (via ICSR.PENDSVSET) every time a
@ process's time-slice expires.  It runs at the lowest priority so it never
@ preempts a real interrupt handler.
@
@ Cortex-M0+ exception entry automatically saves the "hardware frame"
@ {r0, r1, r2, r3, r12, lr, pc, xpsr} onto PSP (when Thread mode was using
@ PSP).  PendSV_Handler saves the remaining callee-saved registers
@ {r4–r11} and the adjusted PSP into the outgoing PCB, calls sched_next()
@ to pick the next process, then restores the incoming process's context.
@
@ Stack layout for a suspended process (low→high, PSP = lowest address):
@
@   [PSP+0 ]  r4          ← pcb_t.sp points here
@   [PSP+4 ]  r5
@   [PSP+8 ]  r6
@   [PSP+12]  r7
@   [PSP+16]  r8
@   [PSP+20]  r9
@   [PSP+24]  r10
@   [PSP+28]  r11
@   [PSP+32]  r0   ─┐
@   [PSP+36]  r1    │  hardware exception frame
@   [PSP+40]  r2    │  (saved automatically on exception entry)
@   [PSP+44]  r3    │
@   [PSP+48]  r12   │
@   [PSP+52]  lr    │
@   [PSP+56]  pc    │
@   [PSP+60]  xpsr ─┘
@
@ PCB_SP_OFFSET: byte offset of the `sp` field inside pcb_t.
@ MUST match offsetof(pcb_t, sp); verified by _Static_assert in proc.c.
@ =============================================================================

.equ PCB_SP_OFFSET, 32      @ offsetof(pcb_t, sp) — see proc.h

.syntax unified
.cpu cortex-m0plus
.thumb

.section .text.pendsv, "ax", %progbits
.align 2
.thumb_func
.global PendSV_Handler
PendSV_Handler:

    @ ── Save outgoing context ──────────────────────────────────────────
    mrs  r0, psp              @ r0 = outgoing process stack pointer (PSP)

    @ Make room for r4–r11 (8 × 4 = 32 bytes) below the exception frame.
    @ ARMv6-M only has STMIA (increment after), so we adjust SP first.
    subs r0, r0, #32
    stmia r0!, {r4-r7}        @ save r4–r7;  r0 now points past r7

    @ r8–r11 cannot appear directly in STMIA on ARMv6-M; move via r4–r7.
    mov  r4, r8
    mov  r5, r9
    mov  r6, r10
    mov  r7, r11
    stmia r0!, {r4-r7}        @ save r8–r11 (as r4–r7); r0 points past r11

    @ Rewind r0 to the bottom of the saved context (= new saved PSP).
    subs r0, r0, #32

    @ ── Write saved PSP into the outgoing PCB ──────────────────────────
    ldr  r2, =current         @ r2 = &current  (address of the global pcb_t*)
    ldr  r3, [r2]             @ r3 = current PCB pointer
    str  r0, [r3, #PCB_SP_OFFSET]  @ current->sp = r0

    @ ── Call C scheduler to select next process ────────────────────────
    @ Preserve &current (r2) and EXC_RETURN (lr) across the C call.
    push {r2, lr}
    bl   sched_next           @ returns next pcb_t* in r0
    pop  {r2, r3}             @ r2 = &current, r3 = EXC_RETURN (was lr)

    @ Update the current global and restore EXC_RETURN in lr.
    str  r0, [r2]             @ current = next PCB
    mov  lr, r3               @ restore EXC_RETURN

    @ ── Restore incoming context ───────────────────────────────────────
    ldr  r0, [r0, #PCB_SP_OFFSET]  @ r0 = next->sp (bottom of saved context)

    ldmia r0!, {r4-r7}        @ restore r4–r7; r0 advances to r8 area
    push {r4-r7}              @ preserve real r4–r7 on MSP; use r4–r7 as temps
    ldmia r0!, {r4-r7}        @ load saved r8–r11 into r4–r7; r0 → exc frame
    mov  r8,  r4
    mov  r9,  r5
    mov  r10, r6
    mov  r11, r7
    pop  {r4-r7}              @ recover the real r4–r7

    msr  psp, r0              @ PSP = base of incoming exception frame
    bx   lr                   @ EXC_RETURN: CPU pops exc frame, resumes thread

    @ Literal pool — must be reachable from the LDR above (within ±1 KB).
    .ltorg
