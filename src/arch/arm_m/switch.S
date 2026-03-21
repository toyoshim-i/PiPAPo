@ =============================================================================
@ switch.S — Cortex-M Context Switch (PendSV_Handler)
@ =============================================================================
@
@ PendSV is triggered by SysTick_Handler (via ICSR.PENDSVSET) every time a
@ process's time-slice expires.  It runs at the lowest priority so it never
@ preempts a real interrupt handler.
@
@ The hardware exception entry automatically saves the "hardware frame"
@ {r0, r1, r2, r3, r12, lr, pc, xpsr} onto PSP (when Thread mode was using
@ PSP).  PendSV_Handler saves the remaining callee-saved registers and the
@ adjusted PSP into the outgoing PCB, calls sched_next() to pick the next
@ process, then restores the incoming process's context.
@
@ ARMv6-M (Cortex-M0+) stack layout for a suspended process:
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
@              ...    │  hardware exception frame (8 words)
@   [PSP+60]  xpsr ─┘
@
@ ARMv8-M (Cortex-M33) stack layout with FPU lazy stacking:
@
@   [PSP+0 ]  r4          ← pcb_t.sp points here
@              ...
@   [PSP+28]  r11
@   [PSP+32]  EXC_RETURN  (saved lr — encodes FPU frame type in bit 4)
@   [PSP+36]  s16         ─┐  (only present when EXC_RETURN bit 4 = 0)
@              ...          │  callee-saved FP registers
@   [PSP+96]  s31         ─┘
@   [PSP+36 or 100]  r0   ─┐
@              ...          │  hardware exception frame (8 or 26 words)
@   [PSP+xx]  xpsr        ─┘
@
@ PCB_SP_OFFSET: byte offset of the `sp` field inside pcb_t.
@ MUST match offsetof(pcb_t, sp); verified by _Static_assert in proc.c.
@ =============================================================================

.equ PCB_SP_OFFSET, 32      @ offsetof(pcb_t, sp) — see proc.h
.equ PCB_NS_XOR_OFF, 48     @ offsetof(pcb_t, ns_addr_xor) — see proc.h
.equ PCB_PSP_NS_OFF, 52     @ offsetof(pcb_t, psp_ns_saved) — see proc.h

.syntax unified
#if __ARM_ARCH >= 8
.cpu cortex-m33
.fpu fpv5-sp-d16
#else
.cpu cortex-m0plus
#endif
.thumb

.section .text.pendsv, "ax", %progbits
.align 2
.thumb_func
.global PendSV_Handler
PendSV_Handler:

#if __ARM_ARCH >= 8
    @ ══════════════════════════════════════════════════════════════════════
    @ ARMv8-M (Cortex-M33) path — Thumb-2 + FPU lazy stacking + TrustZone
    @ ══════════════════════════════════════════════════════════════════════

    @ ── Save outgoing context ──────────────────────────────────────────
    @ Check if outgoing process is Non-Secure (EXC_RETURN bit 6 = 0).
    tst  lr, #0x40            @ test S bit (bit 6)
    beq  .Lsave_ns

    @ ── Secure process save ──────────────────────────────────────────
    mrs  r0, psp              @ r0 = outgoing PSP

    @ If the outgoing process used FPU (EXC_RETURN bit 4 = 0),
    @ save callee-saved s16-s31.  The hardware lazy-stacks s0-s15.
    tst  lr, #0x10            @ test EXC_RETURN bit 4
    it   eq
    vstmdbeq r0!, {s16-s31}  @ save s16-s31 if FPU was active

    @ Save r4-r11 and EXC_RETURN (lr).  On M33, stmdb can use any
    @ register in the list — no need for the M0+ r4-r7 dance.
    stmdb r0!, {r4-r11, lr}
    b    .Lsave_done

.Lsave_ns:
    @ ── Non-Secure process save ──────────────────────────────────────
    @ DCRS=0: hardware already pushed r4-r11 + IntegritySignature +
    @ exception frame onto PSP_NS.  We only need to save EXC_RETURN.
    @ On RP2350, PSP_NS is 0x20xxxxxx (already the NS SRAM alias).
    @ Secure code can access 0x20xxxxxx directly (NS MPU PRIVDEFENA).
    mrs  r0, psp_ns
    stmdb r0!, {lr}           @ push EXC_RETURN below HW frames

.Lsave_done:
    @ ── Compute &current_core[core_id] ─────────────────────────────────
    ldr  r1, =core_id_reg
    ldr  r1, [r1]             @ r1 = pointer to core-ID source
    ldr  r1, [r1]             @ r1 = core_id (0 or 1)
    lsls r1, r1, #2           @ r1 = byte offset (0 or 4)
    ldr  r2, =current_core
    adds r2, r2, r1           @ r2 = &current_core[core_id]

    @ ── Write saved PSP into the outgoing PCB ──────────────────────────
    ldr  r3, [r2]             @ r3 = current PCB pointer
    str  r0, [r3, #PCB_SP_OFFSET]  @ current->sp = r0

    @ ── Save PSP_NS for NS processes ─────────────────────────────────
    @ When an NS process blocks in a syscall (via NSC gateway),
    @ PendSV saves the Secure context (PSP_S).  PSP_NS must be
    @ saved separately so another NS process can run on this core.
    ldr  r4, [r3, #PCB_NS_XOR_OFF]
    cbz  r4, .Lskip_psp_ns_save
    mrs  r4, psp_ns
    str  r4, [r3, #PCB_PSP_NS_OFF] @ current->psp_ns_saved = PSP_NS
.Lskip_psp_ns_save:

    @ ── Call C scheduler to select next process ────────────────────────
    @ Preserve &current_core[core_id] (r2) and core_byte_offset (r1).
    @ EXC_RETURN is saved on the process stack, not MSP.
    push {r1, r2}             @ r1 = core_byte_offset, r2 = &current_core
    bl   sched_next           @ r0 = next pcb_t*

    @ ── Update MPU region 2 for incoming process stack ─────────────────
    push {r0, r1}
    bl   mpu_switch
    pop  {r0, r1}

    @ ── Program target HW breakpoints for incoming tracee (if any) ─────
    push {r0, r1}
    bl   trace_arm_hwbp_on_switch
    pop  {r0, r1}

    pop  {r1, r2}             @ r1 = core_byte_offset, r2 = &current_core

    @ Update current_core[core_id].
    str  r0, [r2]             @ current_core[core_id] = next PCB

    @ ── Restore incoming context ───────────────────────────────────────
    @ Check if incoming process is Non-Secure.
    ldr  r3, [r0, #PCB_NS_XOR_OFF] @ r3 = next->ns_addr_xor
    cbz  r3, .Lrestore_secure

    @ NS process — the saved context may be:
    @   (a) NS context (preempted while running NS code): first stacked
    @       word is NS EXC_RETURN (bit 6 = 0).
    @   (b) Secure context (blocked in syscall via NSC gateway): first
    @       stacked word is Secure EXC_RETURN (bit 6 = 1).
    @ Read the first word from pcb->sp to decide which restore path.
    ldr  r4, [r0, #PCB_SP_OFFSET]  @ r4 = pcb->sp (0x20xxxxxx)
    ldr  r4, [r4]                   @ r4 = saved EXC_RETURN
    tst  r4, #0x40                  @ test S bit (bit 6)
    beq  .Lrestore_ns               @ bit clear → NS EXC_RETURN → NS context

    @ ── NS process in Secure syscall: restore PSP_NS + Secure context ─
    @ PSP_NS was saved separately since another NS process may have
    @ run on this core and changed it.
    ldr  r3, [r0, #PCB_PSP_NS_OFF] @ r3 = saved PSP_NS
    msr  psp_ns, r3
    @ Fall through to Secure restore

.Lrestore_secure:
    @ ── Secure process restore ───────────────────────────────────────
    ldr  r0, [r0, #PCB_SP_OFFSET]  @ r0 = next->sp

    ldmia r0!, {r4-r11, lr}  @ restore r4-r11 + EXC_RETURN

    @ If the incoming process used FPU (EXC_RETURN bit 4 = 0),
    @ restore callee-saved s16-s31.
    tst  lr, #0x10
    it   eq
    vldmiaeq r0!, {s16-s31}  @ restore s16-s31 if FPU was active

    msr  psp, r0              @ PSP = base of incoming exception frame
    bx   lr                   @ EXC_RETURN: CPU pops exc frame, resumes thread

.Lrestore_ns:
    @ ── Non-Secure process restore (preempted from NS code) ──────────
    @ Stack layout: {EXC_RETURN, [HW integrity frame], [HW exc frame]}
    @ On RP2350, pcb->sp is 0x20xxxxxx (NS SRAM alias, accessible from
    @ Secure code via NS MPU PRIVDEFENA).  No XOR needed.
    ldr  r0, [r0, #PCB_SP_OFFSET]  @ r0 = next->sp (0x20xxxxxx)
    ldmia r0!, {lr}           @ pop EXC_RETURN
    msr  psp_ns, r0           @ restore PSP_NS (0x20xxxxxx = NS alias)

    @ Set PSP_S to per-core Secure stack for gateway SVC frame.
    ldr  r0, =secure_psp_top
    adds r0, r0, r1           @ r1 = core_byte_offset
    ldr  r0, [r0]
    msr  psp, r0

    @ DEBUG: dump NS return state before bx lr
    push {lr}
    bl   debug_ns_return
    pop  {lr}

    bx   lr                   @ EXC_RETURN (NS): CPU pops integrity + exc frame

#else
    @ ══════════════════════════════════════════════════════════════════════
    @ ARMv6-M (Cortex-M0+) path — Thumb-1 only, no FPU
    @ ══════════════════════════════════════════════════════════════════════

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

    @ ── Compute &current_core[core_id] ──────────────────────────────────
    @ Indirect pointer: core_id_reg → SIO_CPUID (RP2040) or zero (QEMU).
    @ Two loads, no branches — works on both platforms.
    ldr  r1, =core_id_reg
    ldr  r1, [r1]             @ r1 = pointer to core-ID source
    ldr  r1, [r1]             @ r1 = core_id (0 or 1)
    lsls r1, r1, #2           @ r1 = byte offset (0 or 4)
    ldr  r2, =current_core
    adds r2, r2, r1           @ r2 = &current_core[core_id]

    @ ── Write saved PSP into the outgoing PCB ──────────────────────────
    ldr  r3, [r2]             @ r3 = current PCB pointer
    str  r0, [r3, #PCB_SP_OFFSET]  @ current->sp = r0

    @ ── Call C scheduler to select next process ────────────────────────
    @ Preserve &current_core[core_id] (r2) and EXC_RETURN (lr) across calls.
    push {r2, lr}
    bl   sched_next           @ r0 = next pcb_t*

    @ ── Update MPU region 2 for incoming process stack ─────────────────
    @ r0 = next pcb_t*  (argument for mpu_switch — already in place)
    @ Push r0 with padding to maintain 8-byte stack alignment for the call.
    push {r0, r1}             @ r0 = next (arg), r1 = padding
    bl   mpu_switch           @ mpu_switch(next) — reprograms MPU region 2
    pop  {r0, r1}             @ r0 = next pcb_t* restored, r1 discarded

    @ ── Program target HW breakpoints for incoming tracee (if any) ─────
    @ r0 = next pcb_t*  (argument for trace_arm_hwbp_on_switch)
    push {r0, r1}             @ preserve next pointer and stack alignment
    bl   trace_arm_hwbp_on_switch
    pop  {r0, r1}             @ restore next pointer

    pop  {r2, r3}             @ r2 = &current_core[core_id], r3 = EXC_RETURN

    @ Update current_core[core_id] and restore EXC_RETURN.
    str  r0, [r2]             @ current_core[core_id] = next PCB
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

#endif /* __ARM_ARCH >= 8 */

    @ Literal pool — must be reachable from the LDR above (within ±1 KB).
    .ltorg
