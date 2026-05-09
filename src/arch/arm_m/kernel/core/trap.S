@ =============================================================================
@ svc.S — SVC exception handler entry point
@ =============================================================================
@
@ ARM EABI Linux syscall convention (compatible with musl libc):
@   r7    = syscall number (set by caller just before "svc 0")
@   r0–r3 = arguments 1–4 (stacked automatically by Cortex-M0+ hardware)
@   r4    = argument 5 (callee-saved, still live at SVC entry)
@   r5    = argument 6 (callee-saved, still live at SVC entry)
@
@ When SVC fires in Thread mode (using PSP):
@   • Hardware pushes {r0,r1,r2,r3,r12,lr,pc,xpsr} onto PSP.
@   • CPU switches to Handler mode, MSP becomes the active stack.
@   • r4, r5, r7 are NOT in the hardware-saved frame — still live.
@
@ This handler captures r4, r5, r7 BEFORE any C compiler prologue can
@ clobber them, then hands off to syscall_dispatch(frame, nr, a4, a5):
@   r0 = frame  — pointer to PSP (stacked r0–r3 at offsets 0–12)
@   r1 = nr     — syscall number from r7
@   r2 = a4     — 5th argument from r4
@   r3 = a5     — 6th argument from r5
@
@ syscall_dispatch() writes the return value into frame[0] (stacked r0)
@ so the caller sees it in r0 after exception return.
@
@ Post-dispatch checks (in priority order):
@
@   1. exec_pending[core_id] — sys_execve loaded a new process image.
@      Do a full PendSV-style context restore from current->sp so r4-r11
@      (including r9/GOT base) are correct for the new program.
@
@   2. Process not RUNNABLE — the syscall blocked or exited.
@      Non-restart blocking syscalls switch away synchronously inside
@      sched_switch() via arm_kernel_sched_switch(); by the time execution
@      reaches this check again, the same kernel continuation has resumed.
@      Restart-style blocking syscalls (e.g. waitpid) pend PendSV instead;
@      when syscall_restart[core_id] is set, we restore the original frame[0]
@      and adjust the stacked PC back by 2 so SVC re-executes after wakeup.
@
@ All per-core state (exec_pending, syscall_restart, syscall_saved_arg0, current_core)
@ is indexed by core_id via the core_id_reg indirect pointer.
@
@ SVC runs its body on the current process's kernel stack.  After the initial
@ push, the handler saves the post-push MSP in current->svc_msp and switches
@ MSP to current->kernel_sp when needed.  Before any exit path that touches
@ the original push frame (.Lnormal_return's pop, .Lexec_restore's add sp,
@ signal frame fixup at [sp,#20]), MSP is restored from current->svc_msp.
@ =============================================================================

.equ PCB_SP_OFFSET,        32  @ offsetof(pcb_t, sp) — see proc.h
.equ PCB_KERNEL_SP_OFFSET, 36  @ offsetof(pcb_t, kernel_sp)
.equ PCB_SVC_MSP_OFFSET,   40  @ offsetof(pcb_t, svc_msp)
.equ PCB_STATE_OFFSET,     56  @ offsetof(pcb_t, state)
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
    @ --- Capture r4, r5, r6, r7 and EXC_RETURN before any clobber ---
    @ r6 is saved/restored because we use it for core_byte_offset.
    @ 6 words = 24 bytes keeps MSP 8-byte aligned for C calls.
    push  {r3, r4, r5, r6, r7, lr}
    @ Stack: [sp+0]=pad, [sp+4]=r4, [sp+8]=r5, [sp+12]=r6, [sp+16]=r7, [sp+20]=lr

    @ --- Compute core_byte_offset into r6 (preserved across C calls) ---
    @ Indirect pointer: core_id_reg → SIO_CPUID (RP2040) or zero (QEMU).
    ldr   r6, =core_id_reg
    ldr   r6, [r6]             @ r6 = pointer to core-ID source
    ldr   r6, [r6]             @ r6 = core_id (0 or 1)
    lsls  r6, r6, #2           @ r6 = byte offset (0 or 4)

    @ --- Save EXC_RETURN for signal frame fixup (M33 FPU) ---
#if __ARM_ARCH >= 8
    ldr   r0, [sp, #20]       @ r0 = EXC_RETURN from pushed lr
    ldr   r1, =svc_exc_return
    adds  r1, r1, r6
    str   r0, [r1]             @ svc_exc_return[core_id] = EXC_RETURN
#endif

    @ --- Save original frame[0] for potential syscall restart ---
    mrs   r0, psp              @ r0 = frame pointer on PSP
    ldr   r2, [r0]             @ r2 = original frame[0] (a0)
    ldr   r3, =syscall_saved_arg0
    adds  r3, r3, r6
    str   r2, [r3]             @ syscall_saved_arg0[core_id] = original a0

    @ --- Build argument registers for syscall_dispatch(frame, nr, a4, a5) ---
    @ Reads from [sp, #N] must happen BEFORE the MSP swap below — they
    @ access the pushed frame on the OLD MSP.
    mrs   r0, psp              @ r0 = frame
    ldr   r1, [sp, #16]       @ r1 = saved r7 = syscall number
    ldr   r2, [sp, #4]        @ r2 = saved r4 = 5th arg
    ldr   r3, [sp, #8]        @ r3 = saved r5 = 6th arg

    @ --- Swap MSP to current->kernel_sp ---
    @ r0-r3 hold dispatch args; r6 holds core_byte_offset.  r4-r5 are
    @ scratch (their saved values live on the OLD MSP push frame and are
    @ restored by the matching pop at .Lnormal_return).
    @
    @ The OLD MSP is stashed unconditionally into current->svc_msp so the
    @ unswap at .Lnormal_return is symmetric.  The actual MSP move only
    @ happens when MSP is OUTSIDE the per-process kernel slot (i.e. above
    @ kernel_sp, in the boot stack) — this is the case for the very first
    @ SVC after boot before any PendSV cycle has set MSP into the slot.
    @ Once PendSV has restored MSP from kernel_sp, the post-push MSP is
    @ below kernel_sp and the swap is a no-op (avoiding the overlap with
    @ the entry push frame that would happen if we raised MSP back to
    @ kernel_sp).
    mrs   r4, msp                           @ r4 = OLD MSP (post-push)
    ldr   r5, =current_core
    adds  r5, r5, r6
    ldr   r5, [r5]                          @ r5 = current PCB
    str   r4, [r5, #PCB_SVC_MSP_OFFSET]     @ current->svc_msp = OLD MSP
    ldr   r5, [r5, #PCB_KERNEL_SP_OFFSET]   @ r5 = current->kernel_sp
    cmp   r4, r5
    bls   1f                                @ MSP <= kernel_sp → already in slot
    msr   msp, r5                           @ MSP = kernel_sp (was outside slot)
1:

    @ --- Dispatch ---
    bl    syscall_dispatch     @ writes return value into frame[0] (stacked r0)

    @ --- Check 1: exec_pending[core_id] (sys_execve loaded new image) ---
    @ r6 is callee-saved, still holds core_byte_offset after bl.
    ldr   r0, =exec_pending
    adds  r0, r0, r6
    ldr   r1, [r0]
    cmp   r1, #0
    bne   .Lexec_restore

    @ --- Check 2: process state — did the syscall block/exit? ---
    ldr   r0, =current_core
    adds  r0, r0, r6
    ldr   r0, [r0]                    @ r0 = current PCB
    movs  r1, #PCB_STATE_OFFSET
    ldrb  r1, [r0, r1]               @ r1 = current->state (byte-sized enum)
    cmp   r1, #PROC_RUNNABLE
    bne   .Lcheck_restart

    @ --- Check 3: pending signals (process is RUNNABLE) ---
    @ signal_check() may modify PSP (signal frame setup) or call sys_exit.
    bl    signal_check
    b     .Lnormal_return

.Lcheck_restart:
    @ Process is not runnable.  For non-restart blocking, direct
    @ arm_kernel_sched_switch() has already switched away and later resumed
    @ this SVC continuation.  For restart blocking, PendSV will tail-chain
    @ after exception return and the adjusted SVC will replay after wakeup.
    ldr   r0, =syscall_restart
    adds  r0, r0, r6
    ldr   r1, [r0]
    cmp   r1, #0
    beq   .Lnormal_return             @ no restart — just return

    @ --- Syscall restart: restore frame[0] and adjust PC-2 ---
    movs  r1, #0
    str   r1, [r0]                    @ syscall_restart[core_id] = 0

    mrs   r0, psp
    ldr   r1, =syscall_saved_arg0
    adds  r1, r1, r6
    ldr   r1, [r1]
    str   r1, [r0, #0]               @ frame[0] = original a0 (pid arg etc.)
    ldr   r1, [r0, #24]              @ frame[6] = stacked PC
    subs  r1, #2                     @ PC -= 2 (back to "svc 0" instruction)
    str   r1, [r0, #24]

    @ Fall through to normal return — PendSV tail-chains and saves
    @ the adjusted context.  When rescheduled, SVC re-executes.

.Lnormal_return:
    @ --- Swap MSP back to OLD MSP ---
    @ Restore from current->svc_msp before touching the
    @ original push frame on the OLD MSP.
    ldr   r0, =current_core
    adds  r0, r0, r6
    ldr   r0, [r0]
    ldr   r0, [r0, #PCB_SVC_MSP_OFFSET]
    msr   msp, r0

    @ --- Normal return from exception ---
#if __ARM_ARCH >= 8
    @ Restore potentially-modified EXC_RETURN (signal delivery may have
    @ changed it to force a basic frame, sigreturn restores the original).
    ldr   r0, =svc_exc_return
    adds  r0, r0, r6
    ldr   r0, [r0]
    str   r0, [sp, #20]       @ update stacked lr (= EXC_RETURN)
#endif
    @ If restart blocking pended PendSV, it tail-chains immediately on the
    @ bx-lr return below.  Non-restart blocking has already performed the
    @ synchronous continuation switch.
    pop   {r3, r4, r5, r6, r7, pc}
    @ Restore callee-saved r4, r5, r6, r7 (r3 is padding).
    @ pc = EXC_RETURN → exception return to Thread mode.
    @ If PendSV pending, it tail-chains immediately.

.Lexec_restore:
    @ --- Swap MSP back to OLD MSP ---
    @ Same as .Lnormal_return: the body below pops/skips the original
    @ push frame on the OLD MSP.
    ldr   r1, =current_core
    adds  r1, r1, r6
    ldr   r1, [r1]
    ldr   r1, [r1, #PCB_SVC_MSP_OFFSET]
    msr   msp, r1

    @ --- sys_execve: full context restore from new process image ---
    @ r0 = &exec_pending[core_id] (from check above)
    @ Clear the flag
    movs  r1, #0
    str   r1, [r0]                    @ exec_pending[core_id] = 0

    @ Load current PCB
    ldr   r0, =current_core
    adds  r0, r0, r6
    ldr   r0, [r0]                    @ r0 = current PCB

#if __ARM_ARCH >= 8
    @ ARMv8-M: SW frame is {r4-r11, EXC_RETURN} (9 words).
    @ New process hasn't used FPU, so no s16-s31 to restore.
    ldr   r0, [r0, #PCB_SP_OFFSET]  @ r0 = current->sp (SW frame base)
    ldmia r0!, {r4-r11, lr}  @ restore r4-r11 + EXC_RETURN from SW frame

    @ Set PSP to the new HW exception frame
    msr   psp, r0             @ PSP = SW frame + 36 = HW frame base

    @ Return from exception — skip saved {r3,r4,r5,r6,r7,lr} on MSP.
    @ Use lr (EXC_RETURN from new process) instead of the stacked one.
    add   sp, sp, #24         @ skip all 6 stacked words
    bx    lr                  @ EXC_RETURN from new process → exception return
#else
    @ ARMv6-M: SW frame is {r4-r11, EXC_RETURN} (9 words; Phase 4 prep).
    ldr   r0, [r0, #PCB_SP_OFFSET]   @ r0 = current->sp (SW frame base)
    ldmia r0!, {r4-r7}       @ restore r4-r7; r0 advances to r8 area
    push  {r4-r7}             @ preserve real r4-r7 on MSP
    ldmia r0!, {r4-r7}       @ load saved r8-r11 into r4-r7
    mov   r8,  r4
    mov   r9,  r5             @ ← r9 = GOT base from new process
    mov   r10, r6
    mov   r11, r7
    pop   {r4-r7}             @ recover real r4-r7
    adds  r0, r0, #4          @ skip the SW frame's EXC_RETURN slot

    @ Set PSP to the new HW exception frame
    msr   psp, r0             @ PSP = SW frame + 36 = HW frame base

    @ Return from exception — skip saved {r3,r4,r5,r6,r7} on MSP since we
    @ just loaded the correct r4-r11 from the new process image.  Only pop lr→pc.
    add   sp, sp, #20         @ skip stacked r3, r4, r5, r6, r7
    pop   {pc}                @ pc = EXC_RETURN → exception return
#endif

    @ Literal pool
    .ltorg
