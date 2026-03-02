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
@   1. exec_pending — sys_execve loaded a new process image.  Do a full
@      PendSV-style context restore from current->sp so r4-r11 (including
@      r9/GOT base) are correct for the new program.
@
@   2. Process not RUNNABLE — the syscall blocked (PROC_BLOCKED/ZOMBIE/
@      SLEEPING).  sched_yield() already pended PendSV, which will
@      tail-chain after our exception return and perform the context
@      switch.  If svc_restart is set (e.g. by sys_waitpid), we restore
@      the original frame[0] and adjust the stacked PC back by 2 so the
@      SVC re-executes when the process is rescheduled.
@ =============================================================================

.equ PCB_SP_OFFSET,    32      @ offsetof(pcb_t, sp) — see proc.h
.equ PCB_STATE_OFFSET, 44      @ offsetof(pcb_t, state) — r4..r11(32)+sp(4)+pid(4)+ppid(4)
.equ PROC_RUNNABLE,     1

.syntax unified
.cpu cortex-m0plus
.thumb

.section .text.svc_handler, "ax", %progbits
.align 2
.thumb_func
.global SVC_Handler
SVC_Handler:
    @ --- Capture r4, r5, r7 before any compiler prologue can clobber ---
    push  {r4, r5, r7, lr}  @ [sp]   = r4 (5th arg)
                            @ [sp+4] = r5 (6th arg)
                            @ [sp+8] = r7 (syscall nr)
                            @ [sp+12]= lr (EXC_RETURN — 0xFFFFFFFD)

    @ --- Save original frame[0] for potential syscall restart ---
    mrs   r0, psp           @ r0 = frame pointer on PSP
    ldr   r2, [r0]          @ r2 = original frame[0] (a0)
    ldr   r3, =svc_saved_a0
    str   r2, [r3]          @ svc_saved_a0 = original a0

    @ --- Build argument registers for syscall_dispatch(frame, nr, a4, a5) ---
    @     r0 already = frame (PSP)
    ldr   r1, [sp, #8]      @ r1 = saved r7 = syscall number
    ldr   r2, [sp, #0]      @ r2 = saved r4 = 5th arg
    ldr   r3, [sp, #4]      @ r3 = saved r5 = 6th arg

    @ --- Dispatch ---
    bl    syscall_dispatch   @ writes return value into frame[0] (stacked r0)

    @ --- Check 1: exec_pending (sys_execve loaded new image) ---
    ldr   r0, =exec_pending
    ldr   r1, [r0]
    cmp   r1, #0
    bne   .Lexec_restore

    @ --- Check 2: process state — did the syscall block/exit? ---
    ldr   r0, =current
    ldr   r0, [r0]                  @ r0 = current PCB
    ldr   r1, [r0, #PCB_STATE_OFFSET] @ r1 = current->state
    cmp   r1, #PROC_RUNNABLE
    bne   .Lcheck_restart

    @ --- Check 3: pending signals (process is RUNNABLE) ---
    @ signal_check() may modify PSP (signal frame setup) or call sys_exit.
    bl    signal_check
    b     .Lnormal_return

.Lcheck_restart:
    @ Process is not runnable.  PendSV (already pended by sched_yield)
    @ will tail-chain after our exception return and switch context.
    @ Check if we need to restart the syscall on next schedule.
    ldr   r0, =svc_restart
    ldr   r1, [r0]
    cmp   r1, #0
    beq   .Lnormal_return           @ no restart — just return

    @ --- Syscall restart: restore frame[0] and adjust PC-2 ---
    movs  r1, #0
    str   r1, [r0]                  @ svc_restart = 0

    mrs   r0, psp
    ldr   r1, =svc_saved_a0
    ldr   r1, [r1]
    str   r1, [r0, #0]             @ frame[0] = original a0 (pid arg etc.)
    ldr   r1, [r0, #24]            @ frame[6] = stacked PC
    subs  r1, #2                   @ PC -= 2 (back to "svc 0" instruction)
    str   r1, [r0, #24]

    @ Fall through to normal return — PendSV tail-chains and saves
    @ the adjusted context.  When rescheduled, SVC re-executes.

.Lnormal_return:
    @ --- Normal return from exception ---
    pop   {r4, r5, r7, pc}  @ restore callee-saved r4, r5, r7
                             @ pc  = EXC_RETURN → exception return to Thread mode
                             @ If PendSV pending, it tail-chains immediately.

.Lexec_restore:
    @ --- sys_execve: full context restore from new process image ---
    @ Clear the flag
    movs  r1, #0
    str   r1, [r0]

    @ Load current->sp (points at the new SW frame built by do_execve)
    ldr   r0, =current
    ldr   r0, [r0]           @ r0 = current PCB
    ldr   r0, [r0, #PCB_SP_OFFSET]  @ r0 = current->sp (SW frame base)

    @ Restore r4-r11 from the SW frame (same code as PendSV_Handler)
    ldmia r0!, {r4-r7}       @ restore r4-r7; r0 advances to r8 area
    push  {r4-r7}             @ preserve real r4-r7 on MSP
    ldmia r0!, {r4-r7}       @ load saved r8-r11 into r4-r7
    mov   r8,  r4
    mov   r9,  r5             @ ← r9 = GOT base from new process
    mov   r10, r6
    mov   r11, r7
    pop   {r4-r7}             @ recover real r4-r7

    @ Set PSP to the new HW exception frame
    msr   psp, r0             @ PSP = SW frame + 32 = HW frame base

    @ Return from exception — skip saved {r4,r5,r7} on MSP since we just
    @ loaded the correct r4-r11 from the new process image.  Only pop lr→pc.
    add   sp, sp, #12         @ skip stacked r4, r5, r7
    pop   {pc}                @ pc = EXC_RETURN → exception return

    @ Literal pool
    .ltorg
