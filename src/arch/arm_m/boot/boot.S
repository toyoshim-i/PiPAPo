@ =============================================================================
@ startup.S — PiPAPo Reset Handler and Vector Table
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
@   14:    Reserved (PendSV retired in Phase B)
@   15:    SysTick_Handler
@   16-41: IRQ0-IRQ25 (RP2040 has 26 external interrupts)
@ =============================================================================

.syntax unified
#if __ARM_ARCH >= 8
.cpu cortex-m33
#else
.cpu cortex-m0plus
#endif
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
    .word HardFault_Handler     @ 3:  HardFault
#if __ARM_ARCH >= 8
    .word Default_Handler       @ 4:  MemManage
    .word Default_Handler       @ 5:  BusFault
    .word Default_Handler       @ 6:  UsageFault
    .word Default_Handler       @ 7:  Reserved (ARMv8-M)
#else
    .word 0                     @ 4:  Reserved (ARMv6-M)
    .word 0                     @ 5:  Reserved
    .word 0                     @ 6:  Reserved
    .word 0                     @ 7:  Reserved
#endif
    .word 0                     @ 8:  Reserved
    .word 0                     @ 9:  Reserved
    .word 0                     @ 10: Reserved
    .word SVC_Handler           @ 11: SVCall  (svc.S — syscall dispatch)
    .word 0                     @ 12: Reserved
    .word 0                     @ 13: Reserved
    .word Default_Handler       @ 14: PendSV — retired in Phase B
    .word SysTick_Handler       @ 15: SysTick (sched.c — tick + switch_pending check)

    @ --- External interrupts: IRQ0-IRQ25 (RP2040) / IRQ0-IRQ51 (RP2350) ---
    .word UART0RX_IRQ_Handler   @ IRQ0  — UART0 RX on mps2-an500 (uart_qemu.c)
    .word Default_Handler       @ IRQ1  — UART0 TX on mps2-an500 (unused)
    @ IRQ2-IRQ19
    .rept 18
    .word Default_Handler
    .endr
    .word UART0_IRQ_Handler     @ IRQ20 = UART0 on RP2040 (uart.c — uart_init arms it)
    @ IRQ21-IRQ25
    .rept 5
    .word Default_Handler
    .endr
#if __ARM_ARCH >= 8
    @ --- Additional IRQs for RP2350 (Cortex-M33): IRQ26-IRQ51 ---
    @ IRQ26-IRQ32
    .rept 7
    .word Default_Handler
    .endr
    .word UART0_IRQ_Handler     @ IRQ33 = UART0 on RP2350
    @ IRQ34-IRQ51
    .rept 18
    .word Default_Handler
    .endr
#endif


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
    @ Enable FPU on Cortex-M33 (ARMv8-M) — CP10+CP11 full access
    @ Without this, any compiler-generated VFP instruction HardFaults.
    @ -----------------------------------------------------------------
#if __ARM_ARCH >= 8
    ldr  r0, =0xE000ED88       @ CPACR
    ldr  r1, [r0]
    orr  r1, r1, #(0xF << 20)  @ CP10+CP11 = full access
    str  r1, [r0]
    dsb
    isb

    @ Enable FPU lazy stacking — hardware saves s0-s15 on exception
    @ entry only when the handler actually touches FP registers.
    @ ASPEN (bit 31): automatic state preservation enable
    @ LSPEN (bit 30): lazy state preservation enable
    ldr  r0, =0xE000EF34       @ FPCCR
    ldr  r1, [r0]
    orr  r1, r1, #(3 << 30)    @ ASPEN + LSPEN
    str  r1, [r0]
#endif

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

@ =============================================================================
@ HardFault_Handler
@
@ On Cortex-M0+, all faults (undefined instruction, bus error, etc.)
@ escalate to HardFault.  This handler:
@   1. Checks EXC_RETURN bit 2 to determine if fault was in Thread mode
@      (user process, using PSP) or Handler mode (kernel, using MSP).
@   2. For user faults: calls arm_crash_handler() which prints a crash
@      report, kills the process via sys_exit(128+sig), and patches the
@      PSP exception frame PC so returning won't re-fault.
@   3. Returns from exception.  sys_exit() already pended PendSV, which
@      tail-chains after HardFault returns and performs the context switch.
@   4. For kernel faults: falls through to Default_Handler (infinite loop).
@
@ PSP exception frame layout (hardware-saved on fault):
@   [PSP+0]  r0      [PSP+16] r12
@   [PSP+4]  r1      [PSP+20] lr
@   [PSP+8]  r2      [PSP+24] pc  ← faulting instruction
@   [PSP+12] r3      [PSP+28] xpsr
@ =============================================================================

.align 2
.thumb_func
.global HardFault_Handler
HardFault_Handler:
    @ Check EXC_RETURN in lr: bit 2 indicates which stack was in use.
    @   lr = 0xFFFFFFF9 → MSP (handler/kernel mode)
    @   lr = 0xFFFFFFFD → PSP (thread/user mode)
    movs r0, #4
    mov  r1, lr
    tst  r0, r1
    bne  .Luser_fault
    @ ── Kernel-mode HardFault ───────────────────────────────────────
    @ Dump MSP exception frame to UART before halting.
    @ MSP points at {r0,r1,r2,r3,r12,lr,pc,xpsr} stacked by HW.
    mrs  r0, msp                @ r0 = MSP frame pointer
    bl   kernel_hardfault_dump  @ C handler prints to UART
    b    Default_Handler        @ halt after dump

.Luser_fault:

    @ ── User-mode fault ──────────────────────────────────────────────
    mrs  r0, psp                @ r0 = pointer to stacked {r0..xpsr}

    @ Save callee-saved registers on MSP for crash report.
    @ 6 + 4 = 10 words = 40 bytes (8-byte aligned for C calls).
    push {r3, r4, r5, r6, r7, lr}  @ r3 = padding for alignment
    mov  r4, r8
    mov  r5, r9
    mov  r6, r10
    mov  r7, r11
    push {r4-r7}                @ save r8-r11 (4 words)

    @ Call arm_crash_handler(psp_frame, callee_regs)
    @ Calls sys_exit(128+sig) which marks ZOMBIE and pends PendSV.
    mov  r1, sp                 @ r1 = callee_regs on MSP
    bl   arm_crash_handler

    @ ── Return from exception ────────────────────────────────────────
    @ sys_exit() pended PendSV.  Restore our callee-saved registers and
    @ return via EXC_RETURN.  PendSV tail-chains immediately: it saves
    @ the dead process's context (harmless) and switches to the parent.
    pop  {r4-r7}
    mov  r8,  r4
    mov  r9,  r5
    mov  r10, r6
    mov  r11, r7
    pop  {r3, r4, r5, r6, r7, pc}  @ pc = EXC_RETURN → PendSV tail-chains

@ Weak aliases: real handlers are provided by the driver for each build target.
@ - UART0_IRQ_Handler:   uart.c (hardware RP2040, IRQ 20)
@ - UART0RX_IRQ_Handler: uart_qemu.c (QEMU mps2-an500, IRQ 1)
.weak UART0_IRQ_Handler
.thumb_set UART0_IRQ_Handler, Default_Handler
.weak UART0RX_IRQ_Handler
.thumb_set UART0RX_IRQ_Handler, Default_Handler
