@ =============================================================================
@ startup.S — PicoPiAndPortable Reset Handler and Vector Table
@ =============================================================================
@
@ Provides:
@   .vectors section  → placed at 0x10001000 by ppap.ld (FLASH_KERNEL start)
@   Reset_Handler     → copies .data, zeros .bss, calls kmain()
@   Default_Handler   → infinite loop for unhandled exceptions / IRQs
@
@ The RP2040 boot ROM executes the SDK boot2 (first 256 B of flash), which
@ configures XIP/QSPI and exits by reading {SP, entry} from 0x10000100.
@ stage1.S sits there: it sets VTOR=0x10001000, then reads {SP, Reset_Handler}
@ from this vector table and branches to Reset_Handler.
@
@ Cortex-M0+ vector table layout (ARMv6-M):
@   0:     Initial Stack Pointer value
@   1:     Reset_Handler
@   2:     NMI_Handler
@   3:     HardFault_Handler
@   4-10:  Reserved (zero)
@   11:    SVCall_Handler
@   12-13: Reserved (zero)
@   14:    PendSV_Handler
@   15:    SysTick_Handler
@   16-41: IRQ0-IRQ25 (RP2040 has 26 external interrupts)
@ =============================================================================

.syntax unified
.cpu cortex-m0plus
.thumb

@ =============================================================================
@ Interrupt Vector Table
@ =============================================================================

.section .vectors, "a", %progbits
.align 2
.global _vectors
_vectors:
    @ --- Cortex-M system exceptions ---
    .word __stack_top           @ 0:  Initial SP (loaded by CPU on reset)
    .word Reset_Handler         @ 1:  Reset
    .word Default_Handler       @ 2:  NMI
    .word Default_Handler       @ 3:  HardFault
    .word 0                     @ 4:  Reserved
    .word 0                     @ 5:  Reserved
    .word 0                     @ 6:  Reserved
    .word 0                     @ 7:  Reserved
    .word 0                     @ 8:  Reserved
    .word 0                     @ 9:  Reserved
    .word 0                     @ 10: Reserved
    .word Default_Handler       @ 11: SVCall  (Step 8: replaced by SVC_Handler)
    .word 0                     @ 12: Reserved
    .word 0                     @ 13: Reserved
    .word PendSV_Handler        @ 14: PendSV  (switch.S — context switch)
    .word SysTick_Handler       @ 15: SysTick (sched.c — tick + PendSV trigger)

    @ --- RP2040 external interrupts: IRQ0-IRQ25 ---
    @ IRQ0-IRQ19
    .rept 20
    .word Default_Handler
    .endr
    .word UART0_IRQ_Handler     @ IRQ20 = UART0 (uart.c — uart_init_irq arms it)
    @ IRQ21-IRQ25
    .rept 5
    .word Default_Handler
    .endr


@ =============================================================================
@ Reset_Handler
@
@ The CPU jumps here on power-on / reset.  SP is already set to __stack_top
@ by the hardware (loaded from _vectors[0]) before Reset_Handler is called.
@
@ Sequence:
@   1. Copy .data image from flash (LMA = __data_load) to SRAM (VMA)
@   2. Zero the .bss region
@   3. Call kmain() — must never return
@ =============================================================================

.section .text.reset_handler, "ax", %progbits
.align 2
.thumb_func
.global Reset_Handler
Reset_Handler:

    @ -----------------------------------------------------------------
    @ Copy .data: flash image → SRAM
    @   r0 = destination pointer (__data_start, increments)
    @   r1 = destination end    (__data_end,   loop guard)
    @   r2 = source pointer     (__data_load,  increments)
    @ -----------------------------------------------------------------
    ldr  r0, =__data_start
    ldr  r1, =__data_end
    ldr  r2, =__data_load
    b    .Lcopy_check
.Lcopy_loop:
    ldr  r3, [r2]
    adds r2, r2, #4
    str  r3, [r0]
    adds r0, r0, #4
.Lcopy_check:
    cmp  r0, r1
    blo  .Lcopy_loop

    @ -----------------------------------------------------------------
    @ Copy .ramfunc: flash image → SRAM
    @   Same pattern as .data copy; moves RAM-resident code into SRAM.
    @ -----------------------------------------------------------------
    ldr  r0, =__ramfunc_start
    ldr  r1, =__ramfunc_end
    ldr  r2, =__ramfunc_load
    b    .Lramfunc_check
.Lramfunc_loop:
    ldr  r3, [r2]
    adds r2, r2, #4
    str  r3, [r0]
    adds r0, r0, #4
.Lramfunc_check:
    cmp  r0, r1
    blo  .Lramfunc_loop

    @ -----------------------------------------------------------------
    @ Zero .bss
    @   r0 = pointer (__bss_start, increments)
    @   r1 = end     (__bss_end,   loop guard)
    @   r2 = zero value
    @ -----------------------------------------------------------------
    ldr  r0, =__bss_start
    ldr  r1, =__bss_end
    movs r2, #0
    b    .Lzero_check
.Lzero_loop:
    str  r2, [r0]
    adds r0, r0, #4
.Lzero_check:
    cmp  r0, r1
    blo  .Lzero_loop

    @ -----------------------------------------------------------------
    @ Enter kernel — kmain() must never return
    @ -----------------------------------------------------------------
    bl   kmain

    @ Halt if kmain() returns unexpectedly (should never happen)
.Lhalt:
    b    .Lhalt


@ =============================================================================
@ Default_Handler
@
@ Catches all unhandled exceptions and external IRQs.
@ Halts in a tight loop — attach a debugger to inspect PC/LR/xPSR.
@ =============================================================================

.section .text, "ax", %progbits
.align 2
.thumb_func
.global Default_Handler
Default_Handler:
    b    Default_Handler

@ Weak alias: uart.c provides the real UART0_IRQ_Handler for the hardware
@ build.  The QEMU build (uart_qemu.c) never enables this IRQ, so the
@ alias to Default_Handler is a safe fallback.
.weak UART0_IRQ_Handler
.thumb_set UART0_IRQ_Handler, Default_Handler
