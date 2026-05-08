@ =============================================================================
@ switch.S — Cortex-M context switching
@ =============================================================================
@
@ ARM has two switch paths:
@
@   - PendSV_Handler: asynchronous preemption and restart-style syscall
@     blocking.  PendSV is pended by SysTick or arch_yield(), runs at the
@     lowest priority, and must not preempt active SVC execution.
@
@   - arm_kernel_sched_switch: synchronous continuation blocking from an SVC
@     syscall that is already running on the current process's kernel stack.
@     It saves the live MSP call chain and resumes only when that same
@     process is scheduled again.
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
@
@ Both paths save/restore kernel_sp.  At switch entry, MSP is wherever the
@ outgoing handler or SVC continuation landed, typically inside the current
@ process's kernel-stack slot.  Saving that MSP into kernel_sp preserves the
@ slot's live top; restoring next->kernel_sp places MSP back into the incoming
@ process's slot, so subsequent IRQ or SVC entry runs on that slot rather than
@ the shared boot stack.
@ =============================================================================

.equ PCB_SP_OFFSET,        32  @ offsetof(pcb_t, sp) — see proc.h
.equ PCB_KERNEL_SP_OFFSET, 36  @ offsetof(pcb_t, kernel_sp)

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
.global arm_kernel_sched_switch
arm_kernel_sched_switch:
    @ Synchronous switch for a blocked SVC continuation.  This is not an
    @ exception return path: the caller is already running on MSP inside the
    @ current process's kernel stack, and expects to resume after this call.
    cpsid i

#if __ARM_ARCH >= 8
    @ Save the in-flight kernel continuation on the current MSP.
    stmdb sp!, {r4-r11, lr}

    @ Compute &current_core[core_id].
    ldr  r1, =core_id_reg
    ldr  r1, [r1]
    ldr  r1, [r1]
    lsls r1, r1, #2
    ldr  r2, =current_core
    adds r2, r2, r1

    @ Save current MSP + PSP into the outgoing PCB and mark it as a
    @ kernel continuation rather than a normal Thread/PSP frame.
    ldr  r3, [r2]
    mrs  r0, msp
    str  r0, [r3, #PCB_KERNEL_SP_OFFSET]
    mrs  r0, psp
    str  r0, [r3, #PCB_SP_OFFSET]
    mov  r0, r3
    bl   arm_mark_kernel_context

    @ Pick and prepare the incoming process while still on the outgoing
    @ kernel stack.  These temporary call frames are intentionally discarded.
    bl   sched_next
    mov  r4, r0
    bl   mpu_switch
    mov  r0, r4
    bl   trace_arm_hwbp_on_switch

    @ Recompute &current_core[core_id] after the C calls and publish next.
    ldr  r1, =core_id_reg
    ldr  r1, [r1]
    ldr  r1, [r1]
    lsls r1, r1, #2
    ldr  r2, =current_core
    adds r2, r2, r1
    str  r4, [r2]

    mov  r0, r4
    bl   arm_take_kernel_context
    cmp  r0, #0
    bne  .Larm_kernel_restore_kernel_v8

    @ Incoming process resumes via a normal exception return to Thread/PSP.
    ldr  r0, [r4, #PCB_KERNEL_SP_OFFSET]
    msr  msp, r0
    ldr  r0, [r4, #PCB_SP_OFFSET]
    ldmia r0!, {r4-r11, lr}
    tst  lr, #0x10
    it   eq
    vldmiaeq r0!, {s16-s31}
    msr  psp, r0
    cpsie i
    bx   lr

.Larm_kernel_restore_kernel_v8:
    ldr  r0, [r4, #PCB_SP_OFFSET]
    msr  psp, r0
    ldr  r0, [r4, #PCB_KERNEL_SP_OFFSET]
    msr  msp, r0
    ldmia sp!, {r4-r11, lr}
    cpsie i
    bx   lr
#else
    @ Save the in-flight kernel continuation on the current MSP.
    sub  sp, #36
    mov  r0, sp
    stmia r0!, {r4-r7}
    mov  r4, r8
    mov  r5, r9
    mov  r6, r10
    mov  r7, r11
    stmia r0!, {r4-r7}
    mov  r4, lr
    str  r4, [r0]

    @ Compute &current_core[core_id].
    ldr  r1, =core_id_reg
    ldr  r1, [r1]
    ldr  r1, [r1]
    lsls r1, r1, #2
    ldr  r2, =current_core
    adds r2, r2, r1

    @ Save current MSP + PSP into the outgoing PCB and mark it as a
    @ kernel continuation rather than a normal Thread/PSP frame.
    ldr  r3, [r2]
    mov  r0, sp
    str  r0, [r3, #PCB_KERNEL_SP_OFFSET]
    mrs  r0, psp
    str  r0, [r3, #PCB_SP_OFFSET]
    mov  r0, r3
    bl   arm_mark_kernel_context

    @ Pick and prepare the incoming process while still on the outgoing
    @ kernel stack.  These temporary call frames are intentionally discarded.
    bl   sched_next
    mov  r4, r0
    bl   mpu_switch
    mov  r0, r4
    bl   trace_arm_hwbp_on_switch

    @ Recompute &current_core[core_id] after the C calls and publish next.
    ldr  r1, =core_id_reg
    ldr  r1, [r1]
    ldr  r1, [r1]
    lsls r1, r1, #2
    ldr  r2, =current_core
    adds r2, r2, r1
    str  r4, [r2]

    mov  r0, r4
    bl   arm_take_kernel_context
    cmp  r0, #0
    bne  .Larm_kernel_restore_kernel_v6

    @ Incoming process resumes via a normal exception return to Thread/PSP.
    ldr  r0, [r4, #PCB_KERNEL_SP_OFFSET]
    msr  msp, r0
    ldr  r0, [r4, #PCB_SP_OFFSET]
    ldmia r0!, {r4-r7}
    push {r4-r7}
    ldmia r0!, {r4-r7}
    mov  r8,  r4
    mov  r9,  r5
    mov  r10, r6
    mov  r11, r7
    ldr  r4, [r0]
    mov  lr, r4
    adds r0, r0, #4
    pop  {r4-r7}
    msr  psp, r0
    cpsie i
    bx   lr

.Larm_kernel_restore_kernel_v6:
    ldr  r0, [r4, #PCB_SP_OFFSET]
    msr  psp, r0
    ldr  r0, [r4, #PCB_KERNEL_SP_OFFSET]
    msr  msp, r0
    mov  r0, sp
    mov  r1, r0
    adds r1, #16
    ldmia r1!, {r4-r7}
    mov  r8,  r4
    mov  r9,  r5
    mov  r10, r6
    mov  r11, r7
    ldr  r4, [r1]
    mov  lr, r4
    mov  r1, r0
    ldmia r1!, {r4-r7}
    adds r0, r0, #36
    msr  msp, r0
    cpsie i
    bx   lr
#endif

    .ltorg

.align 2
.thumb_func
.global PendSV_Handler
PendSV_Handler:
    @ Asynchronous exception-return switch.  The interrupted Thread context
    @ lives on PSP; PendSV saves the software frame, picks the next process,
    @ and returns through that process's exception frame.

#if __ARM_ARCH >= 8
    @ ══════════════════════════════════════════════════════════════════════
    @ ARMv8-M (Cortex-M33) path — Thumb-2 + FPU lazy stacking
    @ ══════════════════════════════════════════════════════════════════════

    @ ── Save outgoing context ──────────────────────────────────────────
    mrs  r0, psp              @ r0 = outgoing PSP

    @ If the outgoing process used FPU (EXC_RETURN bit 4 = 0),
    @ save callee-saved s16-s31.  The hardware lazy-stacks s0-s15.
    tst  lr, #0x10            @ test EXC_RETURN bit 4
    it   eq
    vstmdbeq r0!, {s16-s31}  @ save s16-s31 if FPU was active

    @ Save r4-r11 and EXC_RETURN (lr).  On M33, stmdb can use any
    @ register in the list — no need for the M0+ r4-r7 dance.
    stmdb r0!, {r4-r11, lr}
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

    @ ── Save outgoing MSP into current->kernel_sp ──────────────────────
    @ The MSP value here is wherever the handler chain (SysTick → PendSV
    @ tail-chain typically) landed inside the outgoing proc's kernel slot.
    mrs  r4, msp
    str  r4, [r3, #PCB_KERNEL_SP_OFFSET]

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

    mov  r4, r0
    bl   arm_take_kernel_context
    cmp  r0, #0
    bne  .Larm_kernel_restore_kernel_v8
    mov  r0, r4

    @ ── Restore incoming MSP from next->kernel_sp ──────────────────────
    @ After exception return the CPU resumes Thread mode on PSP, but MSP
    @ stays at whatever we set here — so any subsequent IRQ or SVC for
    @ the incoming process runs on its own kernel-stack slot.
    ldr  r4, [r0, #PCB_KERNEL_SP_OFFSET]
    msr  msp, r4

    @ ── Restore incoming context ───────────────────────────────────────
    ldr  r0, [r0, #PCB_SP_OFFSET]  @ r0 = next->sp

    ldmia r0!, {r4-r11, lr}  @ restore r4-r11 + EXC_RETURN

    @ If the incoming process used FPU (EXC_RETURN bit 4 = 0),
    @ restore callee-saved s16-s31.
    tst  lr, #0x10
    it   eq
    vldmiaeq r0!, {s16-s31}  @ restore s16-s31 if FPU was active

    msr  psp, r0              @ PSP = base of incoming exception frame
    bx   lr                   @ EXC_RETURN: CPU pops exc frame, resumes thread

#else
    @ ══════════════════════════════════════════════════════════════════════
    @ ARMv6-M (Cortex-M0+) path — Thumb-1 only, no FPU
    @ ══════════════════════════════════════════════════════════════════════

    @ ── Save outgoing context ──────────────────────────────────────────
    mrs  r0, psp              @ r0 = outgoing process stack pointer (PSP)

    @ Make room for r4–r11 + EXC_RETURN (9 × 4 = 36 bytes) below the
    @ exception frame.  ARMv6-M only has STMIA (increment after), so
    @ we adjust SP first.
    subs r0, r0, #36
    stmia r0!, {r4-r7}        @ save r4–r7;  r0 now points past r7

    @ r8–r11 cannot appear directly in STMIA on ARMv6-M; move via r4–r7.
    mov  r4, r8
    mov  r5, r9
    mov  r6, r10
    mov  r7, r11
    stmia r0!, {r4-r7}        @ save r8–r11 (as r4–r7); r0 points past r11

    @ Save EXC_RETURN at sw[8] (Phase 4 prep).
    mov  r4, lr
    str  r4, [r0]             @ store EXC_RETURN; r0 unchanged

    @ Rewind r0 to the bottom of the saved context (= new saved PSP).
    @ r0 currently points at sw[8]; subtract 32 to reach sw[0].
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

    @ ── Save outgoing MSP into current->kernel_sp ──────────────────────
    @ See M33 path above for rationale.  r4-r7 are scratch (saved on
    @ PSP via the stmia above).
    mrs  r4, msp
    str  r4, [r3, #PCB_KERNEL_SP_OFFSET]

    @ ── Call C scheduler to select next process ────────────────────────
    @ Preserve &current_core[core_id] (r2).  Outgoing EXC_RETURN was
    @ saved into the SW frame above; r3 is just push-alignment padding.
    push {r2, r3}
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

    pop  {r2, r3}             @ r2 = &current_core[core_id]; r3 discarded

    @ Update current_core[core_id].
    str  r0, [r2]             @ current_core[core_id] = next PCB

    mov  r4, r0
    bl   arm_take_kernel_context
    cmp  r0, #0
    bne  .Larm_kernel_restore_kernel_v6
    mov  r0, r4

    @ ── Restore incoming MSP from next->kernel_sp ──────────────────────
    @ See M33 path above for rationale.
    ldr  r4, [r0, #PCB_KERNEL_SP_OFFSET]
    msr  msp, r4

    @ ── Restore incoming context ───────────────────────────────────────
    ldr  r0, [r0, #PCB_SP_OFFSET]  @ r0 = next->sp (bottom of saved context)

    ldmia r0!, {r4-r7}        @ restore r4–r7; r0 advances to r8 area
    push {r4-r7}              @ preserve real r4–r7 on MSP; use r4–r7 as temps
    ldmia r0!, {r4-r7}        @ load saved r8–r11 into r4–r7; r0 → lr slot
    mov  r8,  r4
    mov  r9,  r5
    mov  r10, r6
    mov  r11, r7

    @ Load EXC_RETURN BEFORE popping the real r4–r7 — `ldr r4, [r0]`
    @ clobbers r4, so we must consume it (into lr) before the pop
    @ restores r4 to its incoming-process value.
    ldr  r4, [r0]             @ r4 = sw[8] = EXC_RETURN
    mov  lr, r4
    adds r0, r0, #4           @ r0 → HW exception frame base

    pop  {r4-r7}              @ recover the real r4–r7

    msr  psp, r0              @ PSP = base of incoming exception frame
    bx   lr                   @ EXC_RETURN: CPU pops exc frame, resumes thread

#endif /* __ARM_ARCH >= 8 */

    @ Literal pool — must be reachable from the LDR above (within ±1 KB).
    .ltorg
