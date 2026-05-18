@ =============================================================================
@ switch.S — Cortex-M context switching (Phase B: unified SW frame shape)
@ =============================================================================
@
@ Two callable switch points share one save/restore shape:
@
@   - arm_kernel_sched_switch: synchronous switch from an SVC syscall
@     body.  Saves the live call chain on MSP at whatever depth the
@     bl reaches; resumes there and unwinds C back to the SVC handler.
@
@   - PendSV_Handler: asynchronous preemption from Thread mode (set by
@     SysTick tick exhaustion or by Thread-mode arch_yield()).  Runs at
@     lowest priority and never preempts SVC.  Saves on MSP at the slot
@     top (since steady-state MSP at PendSV entry = kernel_sp); resumes
@     there and EXC_RETURN's to the user HW frame on PSP.
@
@ Unified SW frame layout (40 bytes, 8-aligned, MSP grows down):
@
@   [MSP+0 ]  r4          ← pcb_t.sp points here
@   [MSP+4 ]  r5
@   [MSP+8 ]  r6
@   [MSP+12]  r7
@   [MSP+16]  r8
@   [MSP+20]  r9
@   [MSP+24]  r10
@   [MSP+28]  r11
@   [MSP+32]  saved lr    (EXC_RETURN for PendSV/fresh; C return addr
@                          for arm_kernel_sched_switch)
@   [MSP+36]  saved PSP   (HW exception frame base on the user stack)
@
@ On Cortex-M33 with FPU, callee-saved s16-s31 sit ABOVE the SW frame
@ (64 bytes) when the suspended process had FPU state active.
@
@ Restore (single path):
@   1. MSR msp = pcb_t.sp.
@   2. Read sw[+36] → MSR psp.
@   3. Pop r4-r11 + lr.
@   4. bx lr — dispatches by saved-lr value:
@        * PendSV/fresh: EXC_RETURN → CPU unstacks HW frame from PSP →
@                        Thread mode at user PC.
@        * arm_kernel_sched_switch: C return addr → continues syscall
@                        body, eventually unwinds to SVC normal_return
@                        which pops its own scratch frame and EXC_RETURN's.
@
@ PCB_SP_OFFSET / PCB_KERNEL_SP_OFFSET / PCB_SAVED_PSP_OFFSET:
@   byte offsets in pcb_t.  Asserted by _Static_assert in proc.c.
@
@ kernel_sp is the IMMUTABLE slot top — planted once, never written by
@ save paths.  See pcb_t.kernel_sp doc in proc_info.h.
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
    @ Synchronous switch from a syscall body running on MSP.  Saves r4-r11
    @ + the C return lr + the live user PSP onto the kernel stack at the
    @ current depth, picks the next process, and restores its saved state
    @ (which lands either back in another syscall body or, for PendSV /
    @ fresh frames, at an EXC_RETURN that unwinds to user mode).
    cpsid i

#if __ARM_ARCH >= 8
    @ Push 10-word SW frame on MSP.  PSP at +36, r4-r11+lr at +0..32.
    mrs   r0, psp
    stmdb sp!, {r0}
    stmdb sp!, {r4-r11, lr}
#else
    @ ARMv6-M: manual layout.
    sub   sp, #40
    mov   r0, sp
    stmia r0!, {r4-r7}            @ r4-r7 at [sp+0..12]; r0 → [sp+16]
    mov   r4, r8
    mov   r5, r9
    mov   r6, r10
    mov   r7, r11
    stmia r0!, {r4-r7}            @ r8-r11 (via r4-r7) at [sp+16..28]; r0 → [sp+32]
    mov   r4, lr
    str   r4, [r0]                @ saved lr at [sp+32]
    mrs   r4, psp
    str   r4, [r0, #4]            @ saved PSP at [sp+36]
#endif

    @ Compute &current_core[core_id] → r2 ; outgoing PCB → r3.
    ldr   r1, =core_id_reg
    ldr   r1, [r1]
    ldr   r1, [r1]
    lsls  r1, r1, #2
    ldr   r2, =current_core
    adds  r2, r2, r1
    ldr   r3, [r2]

    @ pcb_t.sp = saved MSP (= SW frame base).
    mov   r0, sp
    str   r0, [r3, #PCB_SP_OFFSET]

    @ Preserve &current_core (r2) across the C calls (caller-saved in AAPCS).
    push  {r2, r3}

    @ sched_next() → r0 = incoming pcb_t*.
    bl    sched_next

    @ mpu_switch(next) ; preserve r0 (next) across the bl.
    push  {r0, r1}
    bl    mpu_switch
    pop   {r0, r1}

    @ trace_arm_hwbp_on_switch(next).
    push  {r0, r1}
    bl    trace_arm_hwbp_on_switch
    pop   {r0, r1}

    pop   {r2, r3}                @ r2 = &current_core[cid] ; r3 unused

    @ Publish current_core[cid] = next.
    str   r0, [r2]

    @ Single-path restore — matches the unified SW frame shape used by
    @ both arm_kernel_sched_switch save and PendSV save (below).
    ldr   r0, [r0, #PCB_SP_OFFSET]
    msr   msp, r0
#if __ARM_ARCH >= 8
    ldmia sp!, {r4-r11, lr}       @ r4-r11 + saved lr
    ldr   r0, [sp], #4            @ saved PSP
    msr   psp, r0
    @ Conditional FPU restore: only when the saved lr is recognisably an
    @ EXC_RETURN value (top byte 0xFF) — for arm_kernel_sched_switch
    @ frames the saved lr is a C return address and FPU restore would
    @ misread MSP-resident kernel data as FPU state.
    mov   r0, lr
    lsr   r0, r0, #28
    cmp   r0, #0xF
    bne   1f
    tst   lr, #0x10
    it    eq
    vldmiaeq sp!, {s16-s31}
1:
    cpsie i
    bx    lr
#else
    mov   r1, sp                  @ r1 = SW frame base
    adds  r1, #16                 @ r1 → r8 slot
    ldmia r1!, {r4-r7}            @ r4-r7 ← saved r8-r11; r1 → lr slot
    mov   r8,  r4
    mov   r9,  r5
    mov   r10, r6
    mov   r11, r7
    ldr   r4, [r1]                @ r4 = saved lr
    mov   lr, r4
    ldr   r4, [r1, #4]            @ r4 = saved PSP
    msr   psp, r4
    mov   r1, sp
    ldmia r1!, {r4-r7}            @ r4-r7 ← saved r4-r7
    add   sp, #40                 @ pop full 10-word SW frame
    cpsie i
    bx    lr
#endif

    .ltorg

.align 2
.thumb_func
.global PendSV_Handler
PendSV_Handler:
    @ Asynchronous preemption from Thread mode (interrupted user code on
    @ PSP).  HW pushed the user exception frame on PSP.  We push the
    @ unified 10-word SW frame on MSP, then drive the same swap loop as
    @ arm_kernel_sched_switch — both restore paths converge.
    @ PendSV is lowest-priority, so SVC is never preempted here; MSP at
    @ entry is at the current process's kernel slot top (= kernel_sp).

#if __ARM_ARCH >= 8
    @ FPU lazy stacking: save callee-saved s16-s31 above the SW frame
    @ when the interrupted process had FPU state active.  Detected at
    @ restore time via the saved EXC_RETURN's bit 4 (see above).
    tst   lr, #0x10
    it    eq
    vstmdbeq sp!, {s16-s31}

    @ Push 10-word SW frame on MSP: saved PSP at +36, r4-r11+lr at +0..32.
    mrs   r0, psp
    stmdb sp!, {r0}
    stmdb sp!, {r4-r11, lr}
#else
    @ ARMv6-M: manual layout matching arm_kernel_sched_switch's.
    sub   sp, #40
    mov   r0, sp
    stmia r0!, {r4-r7}
    mov   r4, r8
    mov   r5, r9
    mov   r6, r10
    mov   r7, r11
    stmia r0!, {r4-r7}
    mov   r4, lr
    str   r4, [r0]
    mrs   r4, psp
    str   r4, [r0, #4]
#endif

    @ Compute &current_core[core_id] → r2 ; outgoing PCB → r3.
    ldr   r1, =core_id_reg
    ldr   r1, [r1]
    ldr   r1, [r1]
    lsls  r1, r1, #2
    ldr   r2, =current_core
    adds  r2, r2, r1
    ldr   r3, [r2]

    @ pcb_t.sp = saved MSP.
    mov   r0, sp
    str   r0, [r3, #PCB_SP_OFFSET]

    @ sched_next, mpu_switch, trace_arm_hwbp_on_switch (preserve r2).
    push  {r2, r3}
    bl    sched_next
    pop   {r2, r3}

    push  {r0, r2}
    bl    mpu_switch
    pop   {r0, r2}

    push  {r0, r2}
    bl    trace_arm_hwbp_on_switch
    pop   {r0, r2}

    @ Publish current_core[cid] = next.
    str   r0, [r2]

    @ Single-path restore — identical to arm_kernel_sched_switch's.
    ldr   r0, [r0, #PCB_SP_OFFSET]
    msr   msp, r0
#if __ARM_ARCH >= 8
    ldmia sp!, {r4-r11, lr}
    ldr   r0, [sp], #4
    msr   psp, r0
    mov   r0, lr
    lsr   r0, r0, #28
    cmp   r0, #0xF
    bne   1f
    tst   lr, #0x10
    it    eq
    vldmiaeq sp!, {s16-s31}
1:
    bx    lr                      @ EXC_RETURN or C return — see save
#else
    mov   r1, sp
    adds  r1, #16
    ldmia r1!, {r4-r7}
    mov   r8,  r4
    mov   r9,  r5
    mov   r10, r6
    mov   r11, r7
    ldr   r4, [r1]
    mov   lr, r4
    ldr   r4, [r1, #4]
    msr   psp, r4
    mov   r1, sp
    ldmia r1!, {r4-r7}
    add   sp, #40
    bx    lr
#endif

    .ltorg
