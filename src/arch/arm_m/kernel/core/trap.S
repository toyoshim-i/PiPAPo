@ =============================================================================
@ trap.S — SVC exception handler (Phase B: unified 10-word SW frame)
@ =============================================================================
@
@ SVC entry pushes the same 10-word SW frame that arm_kernel_sched_switch
@ and PendSV use (see switch.S).  Layout (40 bytes, 8-aligned, MSP down):
@
@   [sp+0 ]  r4         ← also ldr-d as syscall arg 5 (a4)
@   [sp+4 ]  r5         ← also ldr-d as syscall arg 6 (a5)
@   [sp+8 ]  r6
@   [sp+12]  r7         ← syscall number (nr)
@   [sp+16]  r8
@   [sp+20]  r9
@   [sp+24]  r10
@   [sp+28]  r11
@   [sp+32]  saved lr   (= EXC_RETURN at SVC entry; possibly amended by
@                          signal_check for FPU bit fixup before exit)
@   [sp+36]  saved PSP  (= HW exception frame base on the user stack)
@
@ MSP at SVC entry is the current process's kernel slot top — set by the
@ preceding PendSV / arm_kernel_sched_switch restore.  No scratch frame
@ and no MSP swap; the SW frame directly anchors the kernel-side state.
@
@ ARM EABI Linux syscall convention:
@   r7    = syscall number       (saved at [sp+12])
@   r0-r3 = arguments 1-4        (in HW frame on PSP)
@   r4    = argument 5 (a4)      (saved at [sp+0])
@   r5    = argument 6 (a5)      (saved at [sp+4])
@
@ syscall_dispatch(frame, nr, a4, a5):
@   r0 = frame = PSP (HW frame base)
@   r1 = nr    = saved r7
@   r2 = a4    = saved r4
@   r3 = a5    = saved r5
@
@ Post-dispatch checks (in priority order):
@
@   1. exec_pending[core_id] — sys_execve overwrote this process's SW
@      frame at slot_top-40 with the new image's frame and stashed the
@      new HW frame base at sw[+36].  MSR psp = sw[+36], then fall
@      through to normal return — the pop will lift the new SW frame and
@      bx-lr will EXC_RETURN into the new image.
@
@   2. Process not RUNNABLE — the syscall blocked or exited.  All
@      blocking syscalls do their swap via arm_kernel_sched_switch and
@      have already resumed by the time control reaches here; skip
@      signal_check and fall through.
@
@   3. Pending signals (process is RUNNABLE) → signal_check may push a
@      new HW frame on PSP so EXC_RETURN unwinds into a user handler.
@
@ All per-core state (exec_pending, current_core) is indexed by core_id
@ via the core_id_reg indirect pointer.
@ =============================================================================

.equ PCB_SP_OFFSET,        32  @ offsetof(pcb_t, sp)
.equ PCB_KERNEL_SP_OFFSET, 36  @ offsetof(pcb_t, kernel_sp)
.equ PCB_STATE_OFFSET,     88  @ offsetof(pcb_t, state) — see _Static_assert
.equ PROC_RUNNABLE,         1

.syntax unified
#if __ARM_ARCH >= 8
.cpu cortex-m33
#else
.cpu cortex-m0plus
#endif
.thumb

.section .text.svc_handler, "ax", %progbits
.align 2
.thumb_func
.global SVC_Handler
SVC_Handler:
    @ --- Push unified 10-word SW frame on MSP ---
    @ MSP at entry = current process's kernel_sp (slot top), set by the
    @ preceding PendSV / arm_kernel_sched_switch restore.
#if __ARM_ARCH >= 8
    mrs   r0, psp                @ r0 = HW frame base on user stack
    stmdb sp!, {r0}              @ saved PSP at sw[+36]
    stmdb sp!, {r4-r11, lr}      @ r4-r11 + lr at sw[+0..32]
#else
    sub   sp, #40
    mov   r0, sp
    stmia r0!, {r4-r7}           @ r4-r7 at [sp+0..12]
    mov   r4, r8
    mov   r5, r9
    mov   r6, r10
    mov   r7, r11
    stmia r0!, {r4-r7}           @ r8-r11 (via r4-r7) at [sp+16..28]
    mov   r4, lr
    str   r4, [r0]               @ saved lr at [sp+32]
    mrs   r4, psp
    str   r4, [r0, #4]           @ saved PSP at [sp+36]
#endif

    @ --- Compute core_byte_offset into r6 (callee-saved across bls) ---
    ldr   r6, =core_id_reg
    ldr   r6, [r6]
    ldr   r6, [r6]
    lsls  r6, r6, #2

#if __ARM_ARCH >= 8
    @ --- Save EXC_RETURN for signal frame fixup (M33 FPU) ---
    ldr   r0, [sp, #32]
    ldr   r1, =arm_exc_return
    adds  r1, r1, r6
    str   r0, [r1]
#endif

    @ --- Detect Thread-mode sched_switch sentinel (svc #0xFF) ---
    @ arm_kernel_sched_switch can only run in Handler mode (it pushes on
    @ MSP).  Thread-mode arch_sched_switch issues svc #0xFF so we can
    @ trampoline into Handler mode, swap inline, then return to user.
    mrs   r0, psp
    ldr   r1, [r0, #24]          @ r1 = pc (HW frame slot 6)
    subs  r1, #2                 @ r1 = svc instruction (Thumb is 16-bit)
    ldrh  r2, [r1]               @ r2 = svc instruction encoding
    movs  r3, #0xFF
    ands  r2, r3                 @ r2 = svc immediate
    cmp   r2, #0xFF
    beq   .Lsentinel_yield

    @ --- Build dispatch args: syscall_dispatch(frame, nr, a4, a5) ---
    mrs   r0, psp                @ r0 = frame
    ldr   r1, [sp, #12]          @ r1 = saved r7 = nr
    ldr   r2, [sp, #0]           @ r2 = saved r4 = a4
    ldr   r3, [sp, #4]           @ r3 = saved r5 = a5

    bl    syscall_dispatch       @ writes return value into frame[0]

    @ --- Check 1: exec_pending[core_id] (sys_execve loaded new image) ---
    ldr   r0, =exec_pending
    adds  r0, r0, r6
    ldr   r1, [r0]
    cmp   r1, #0
    bne   .Lexec_pending

    @ --- Check 2: process state — did the syscall block/exit? ---
    ldr   r0, =current_core
    adds  r0, r0, r6
    ldr   r0, [r0]
    movs  r1, #PCB_STATE_OFFSET
    ldrb  r1, [r0, r1]
    cmp   r1, #PROC_RUNNABLE
    bne   .Lnormal_return

    @ --- Check 3: pending signals (process is RUNNABLE) ---
    bl    signal_check

.Lnormal_return:
    @ Restore any saved vfork-parent slice before user-mode EXC_RETURN.
    @ No-op unless current->vfork_frame_saved (set in sys_vfork for the
    @ parent only); see docs/proposals/no_stack_copy_on_vfork.md.
    bl    arm_vfork_restore_frame
#if __ARM_ARCH >= 8
    @ Re-load potentially-modified EXC_RETURN (signal delivery may have
    @ forced a basic frame, sigreturn restores the original).
    ldr   r0, =arm_exc_return
    adds  r0, r0, r6
    ldr   r0, [r0]
    str   r0, [sp, #32]
    pop   {r4-r11, lr}           @ r4-r11 + EXC_RETURN
    add   sp, #4                 @ skip saved PSP slot — PSP is live in PSP reg
    bx    lr                     @ HW frame pops off PSP → Thread mode
#else
    mov   r1, sp                 @ r1 → r4 slot (= SW frame base)
    adds  r1, #16                @ r1 → r8 slot
    ldmia r1!, {r4-r7}           @ r4-r7 ← saved r8-r11; r1 → lr slot
    mov   r8,  r4
    mov   r9,  r5
    mov   r10, r6
    mov   r11, r7
    ldr   r4, [r1]               @ r4 = saved lr = EXC_RETURN
    mov   lr, r4
    pop   {r4-r7}                @ r4-r7 ← saved r4-r7; sp += 16
    add   sp, #24                @ skip r8-r11 + EXC_RETURN + saved PSP (24 bytes)
    bx    lr                     @ HW frame pops off PSP → Thread mode
#endif

.Lsentinel_yield:
    @ Thread-mode arch_sched_switch reached us via svc #0xFF.  Run the
    @ swap inline; when this process resumes here, fall through to
    @ normal_return which pops the SVC entry's SW frame and bx-EXC_RETURN
    @ back to the user PSP — same as a no-op syscall return.
    bl    arm_kernel_sched_switch
    b     .Lnormal_return

.Lexec_pending:
    @ sys_execve overwrote this process's SW frame at slot_top-40 with
    @ the new image's initial state, including the new HW frame base at
    @ sw[+36].  Clear the flag and MSR psp from sw[+36]; then fall
    @ through to the normal pop, which restores r4-r11 (with r9 = new
    @ GOT base) and bx-lr's via the new EXC_RETURN.
    movs  r1, #0
    str   r1, [r0]               @ exec_pending[cid] = 0
    ldr   r0, [sp, #36]          @ r0 = new HW frame base (= sw[+36])
    msr   psp, r0
    b     .Lnormal_return

    .ltorg
