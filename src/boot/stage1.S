@ =============================================================================
@ stage1.S — Stage 1 Bootloader Extension
@ =============================================================================
@
@ Placed at 0x10000100 in the FLASH_BOOT region (immediately after the Pico
@ SDK boot2's 256-byte QSPI init code).
@
@ The SDK boot2 (bs2_default) handles QSPI/XIP initialisation and then
@ exits via exit_from_boot2.S, which:
@   1. Sets VTOR = XIP_BASE + 0x100 = 0x10000100   (points here)
@   2. Reads word 0 from [0x10000100] → MSP (initial stack pointer)
@   3. Reads word 1 from [0x10000104] → PC (Thumb entry, bit 0 set)
@   4. Executes:  msr msp, r0 ; bx r1
@
@ Our header at 0x10000100 presents the two words the SDK exit stub reads.
@ stage1_entry then redirects execution to the final kernel location:
@   1. Set VTOR = 0x10001000  (kernel vector table, FLASH_KERNEL start)
@   2. Load Reset_Handler address from [0x10001004]
@   3. Load __stack_top from [0x10001000]
@   4. Set MSP ; branch to Reset_Handler
@
@ This code must be position-independent Thumb.  It runs with XIP already
@ active (the SDK boot2 enabled it), so the entire flash window is readable.
@ =============================================================================

.equ KERNEL_VTOR, 0x10001000     @ kernel vector table address (FLASH_KERNEL start)
.equ SCB_VTOR,    0xE000ED08     @ Vector Table Offset Register (PPB + 0xED08)

.syntax unified
.cpu cortex-m0plus
.thumb

@ =============================================================================
@ .stage1 header — two words consumed by the SDK boot2 exit stub
@ =============================================================================

.section .stage1, "ax", %progbits
.align 2
.global _stage1
_stage1:
    .word __stack_top       @ [0x10000100] initial SP  — SDK boot2 loads → MSP
    .word stage1_entry      @ [0x10000104] entry point — SDK boot2 jumps here
                            @   (Thumb bit set automatically for .thumb_func)

@ =============================================================================
@ stage1_entry — redirect execution to the kernel at 0x10001000
@
@ On entry:  MSP = __stack_top (already set by SDK boot2)
@            Interrupts disabled (PRIMASK = 1 from reset)
@            XIP active; flash readable at 0x10000000–0x10FFFFFF
@ Clobbers:  r0, r1
@ =============================================================================

.thumb_func
.global stage1_entry
stage1_entry:
    @ -------------------------------------------------------------------
    @ 1. Update VTOR to point at the kernel vector table.
    @    VTOR (Vector Table Offset Register) is at PPB + 0xED08 on M0+.
    @ -------------------------------------------------------------------
    ldr  r0, =KERNEL_VTOR
    ldr  r1, =SCB_VTOR
    str  r0, [r1]               @ VTOR = KERNEL_VTOR

    @ -------------------------------------------------------------------
    @ 2. Load initial SP and Reset_Handler from the kernel vector table.
    @    [0x10001000] = __stack_top    (word 0 of kernel .vectors)
    @    [0x10001004] = Reset_Handler  (word 1, Thumb bit already set)
    @ -------------------------------------------------------------------
    ldr  r1, [r0, #4]           @ r1 = Reset_Handler (with Thumb bit)
    ldr  r0, [r0]               @ r0 = __stack_top

    @ -------------------------------------------------------------------
    @ 3. Set MSP and branch to the kernel entry point.
    @    MSP was already equal to __stack_top from the SDK boot2 exit,
    @    but we repeat it here for correctness in case the two differ.
    @ -------------------------------------------------------------------
    msr  msp, r0
    bx   r1

.ltorg
