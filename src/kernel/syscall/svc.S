@ =============================================================================
@ svc.S — SVC exception handler entry point
@ =============================================================================
@
@ ARM EABI Linux syscall convention:
@   r7  = syscall number (set by caller just before "svc 0")
@   r0–r3 = arguments (stacked automatically by Cortex-M0+ hardware)
@
@ When SVC fires in Thread mode (using PSP):
@   • Hardware pushes {r0,r1,r2,r3,r12,lr,pc,xpsr} onto PSP.
@   • CPU switches to Handler mode, MSP becomes the active stack.
@   • r7 is NOT in the hardware-saved frame — it is still live in r7.
@
@ This handler must capture r7 BEFORE any C compiler prologue can clobber it,
@ then hand off to syscall_dispatch(frame, nr) in C:
@   r0 = frame  — pointer to PSP (stacked r0–r3 at offsets 0–12)
@   r1 = nr     — syscall number from r7
@
@ syscall_dispatch() writes the return value into frame[0] (stacked r0)
@ so the caller sees it in r0 after exception return.
@ =============================================================================

.syntax unified
.cpu cortex-m0plus
.thumb

.section .text.svc_handler, "ax", %progbits
.align 2
.thumb_func
.global SVC_Handler
SVC_Handler:
    @ --- Capture syscall number before any compiler prologue runs ---
    push  {r7, lr}          @ [sp]   = r7 (syscall nr)
                            @ [sp+4] = lr (EXC_RETURN — 0xFFFFFFFD)

    @ --- Build argument registers for syscall_dispatch(frame, nr) ---
    mrs   r0, psp           @ r0 = frame: stacked {r0,r1,r2,r3,...} on PSP
    ldr   r1, [sp, #0]      @ r1 = saved r7 = syscall number

    @ --- Dispatch ---
    bl    syscall_dispatch   @ writes return value into frame[0] (stacked r0)

    @ --- Return from exception ---
    pop   {r7, pc}           @ r7  = restored caller's r7
                             @ pc  = EXC_RETURN → exception return to Thread mode
