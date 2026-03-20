@ =============================================================================
@ stage1.S — Stage 1 Bootloader Extension
@ =============================================================================
@
@ Placed at 0x10000100 in the FLASH_BOOT region (immediately after the Pico
@ SDK boot2's 256-byte QSPI init code).
@
@ Two boot paths use this header:
@
@   1. SWD/JTAG flash path:
@      The SDK boot2 (bs2_default) handles QSPI/XIP and then exits via
@      exit_from_boot2.S, which sets VTOR=0x10000100, reads {SP, entry}
@      from words 0-1, and jumps to stage1_entry.
@
@   2. PicoCalc UF2 bootloader path (pelrun/uf2loader):
@      After writing the UF2 to flash, the bootloader does a watchdog reset.
@      On next boot, stage3 calls launch_application_from(XIP_BASE + 0x100),
@      which also reads {SP, entry} from words 0-1 and jumps to stage1_entry.
@
@      IMPORTANT: The UF2 bootloader writes 28 bytes of proginfo metadata
@      (magic, flash_end, filename) at XIP_BASE + 0x110 = offset +0x10 from
@      this header.  Words 4-10 (the "vector table hole") are reserved for
@      this purpose and must not contain executable code.
@
@ stage1_entry then redirects execution to the final kernel location:
@   1. Set VTOR = __kernel_vtor  (linker-provided, FLASH_KERNEL start)
@   2. Load Reset_Handler address from [__kernel_vtor + 4]
@   3. Load __stack_top from [__kernel_vtor]
@   4. Set MSP ; branch to Reset_Handler
@
@ This code must be position-independent Thumb.  It runs with XIP already
@ active (the SDK boot2 or the UF2 bootloader enabled it).
@ =============================================================================

@ __kernel_vtor is defined by each target's linker script at ORIGIN(FLASH_KERNEL),
@ so stage1 works with any FLASH_KERNEL origin (e.g., 0x10001000 or 0x10004000).
.equ SCB_VTOR,    0xE000ED08     @ Vector Table Offset Register (PPB + 0xED08)

.syntax unified
.cpu cortex-m0plus
.thumb

@ =============================================================================
@ .stage1 header — mimics a Cortex-M vector table at 0x10000100
@
@   Word 0 (+0x00): Initial SP      — consumed by boot2/uf2loader
@   Word 1 (+0x04): Entry point     — consumed by boot2/uf2loader
@   Word 2 (+0x08): Reserved (NMI)
@   Word 3 (+0x0C): Reserved (HardFault)
@   Words 4-10 (+0x10 to +0x2B): Vector table hole — 28 bytes reserved for
@       the UF2 bootloader's proginfo (magic + flash_end + filename).
@       MUST NOT contain executable code; the bootloader overwrites them.
@ =============================================================================

.section .stage1, "ax", %progbits
.align 2
.global _stage1
_stage1:
    .word __stack_top       @ [+0x00] initial SP  — boot2/uf2loader loads → MSP
    .word stage1_entry      @ [+0x04] entry point — boot2/uf2loader jumps here
                            @   (Thumb bit set automatically for .thumb_func)
    .word 0                 @ [+0x08] NMI   — unused (VTOR set to kernel before use)
    .word 0                 @ [+0x0C] HardFault — unused
    .space 28, 0xFF         @ [+0x10 to +0x2B] vector table hole — reserved for
                            @   uf2loader proginfo (overwritten after UF2 flash).
                            @   Must be 0xFF (erased state) because the bootloader
                            @   skips bl_proginfo_clear on the first UF2 block, and
                            @   flash programming can only change bits 1→0.

@ =============================================================================
@ stage1_entry — redirect execution to the kernel at __kernel_vtor
@
@ On entry:  MSP = __stack_top (already set by boot2 or uf2loader)
@            XIP active; flash readable at 0x10000000–0x10FFFFFF
@ Clobbers:  r0, r1
@ =============================================================================

.thumb_func
.global stage1_entry
stage1_entry:
    @ -------------------------------------------------------------------
    @ 0. Disable interrupts immediately.  The UF2 bootloader may leave
    @    IRQs enabled from stage3 (SysTick, I2C, etc.).  Our "vector
    @    table" at 0x10000100 has no proper handlers, so any pending IRQ
    @    would crash.  PRIMASK masks everything except NMI/HardFault.
    @ -------------------------------------------------------------------
    cpsid i

    @ -------------------------------------------------------------------
    @ 1. Update VTOR to point at the kernel vector table.
    @    VTOR (Vector Table Offset Register) is at PPB + 0xED08 on M0+.
    @ -------------------------------------------------------------------
    ldr  r0, =__kernel_vtor
    ldr  r1, =SCB_VTOR
    str  r0, [r1]               @ VTOR = __kernel_vtor

    @ -------------------------------------------------------------------
    @ 2. Load initial SP and Reset_Handler from the kernel vector table.
    @    [__kernel_vtor + 0] = __stack_top    (word 0 of kernel .vectors)
    @    [__kernel_vtor + 4] = Reset_Handler  (word 1, Thumb bit already set)
    @ -------------------------------------------------------------------
    ldr  r1, [r0, #4]           @ r1 = Reset_Handler (with Thumb bit)
    ldr  r0, [r0]               @ r0 = __stack_top

    @ -------------------------------------------------------------------
    @ 3. Set MSP and branch to the kernel entry point.
    @    MSP was already equal to __stack_top from the boot2/uf2loader,
    @    but we repeat it here for correctness in case the two differ.
    @ -------------------------------------------------------------------
    msr  msp, r0
    bx   r1

.ltorg
