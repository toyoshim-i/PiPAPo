@ =============================================================================
@ nsc_veneer.S — Non-Secure Callable syscall gateway for RP2350 TrustZone
@ =============================================================================
@
@ This veneer sits in a SAU region marked NSC (Non-Secure Callable).
@ Non-Secure user processes call here via BLX to make syscalls:
@
@   1. SG   — Secure Gateway: transitions caller from NS to Secure
@   2. SVC  — triggers the Secure SVC_Handler (same as normal syscalls)
@   3. BXNS — returns to Non-Secure caller with r0 = syscall result
@
@ Register convention (same as ARM EABI syscall):
@   r7    = syscall number
@   r0-r3 = arguments 1-4 (passed through registers)
@   r4-r5 = arguments 5-6 (callee-saved, still live)
@
@ The SVC_Handler reads the exception frame from PSP (which is PSP_S in
@ Secure Thread mode) and dispatches normally.  The return value is
@ written to the stacked r0, restored by exception return, and passed
@ back to the NS caller through BXNS.
@
@ Address of this gateway is exported as __nsc_veneer_start by the linker
@ script; user-space _syscall() on pico2 uses it instead of "svc 0".
@ =============================================================================

.syntax unified
.cpu cortex-m33
.thumb

.section .nsc_veneer, "ax", %progbits
.align 5                        @ 32-byte alignment required by SAU regions
.global nsc_syscall_gateway
.thumb_func
nsc_syscall_gateway:
    sg                          @ Secure Gateway — transition NS → Secure
    svc  0                      @ trigger Secure SVC_Handler
    bxns lr                     @ return to Non-Secure caller (lr = FNC_RETURN)
