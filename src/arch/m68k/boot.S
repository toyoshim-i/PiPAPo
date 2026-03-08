| =============================================================================
| boot.S — Motorola 68000 Vector Table and Reset Handler
| =============================================================================
|
| Provides:
|   .vectors section — placed at 0x000000 by the linker script
|   Reset_Handler     — copies .data, zeros .bss, calls kmain()
|   Default_Handler   — infinite loop for unhandled exceptions
|
| 68000 vector table layout (256 entries × 4 bytes = 1024 bytes):
|   0:     Initial SSP (Supervisor Stack Pointer)
|   1:     Initial PC (Reset vector → Reset_Handler)
|   2:     Bus Error
|   3:     Address Error
|   4:     Illegal Instruction
|   5:     Zero Divide
|   6:     CHK instruction
|   7:     TRAPV instruction
|   8:     Privilege Violation
|   9:     Trace
|   10:    Line 1010 (A-line) emulator
|   11:    Line 1111 (F-line) emulator
|   12-23: Reserved
|   24:    Spurious Interrupt
|   25-31: Autovector interrupts (Level 1-7)
|   32-47: TRAP #0 - TRAP #15
|   48-63: Reserved
|   64-255: User interrupt vectors
| =============================================================================

    .section .vectors, "a"
    .align 2

    .global _vectors
_vectors:
    | --- Exception vectors 0-1: Reset ---
    .long __stack_top              | 0: Initial SSP
    .long Reset_Handler            | 1: Initial PC (reset entry point)

    | --- Exception vectors 2-7 ---
    .long Default_Handler          | 2:  Bus Error
    .long Default_Handler          | 3:  Address Error
    .long Default_Handler          | 4:  Illegal Instruction
    .long Default_Handler          | 5:  Zero Divide
    .long Default_Handler          | 6:  CHK
    .long Default_Handler          | 7:  TRAPV

    | --- Exception vectors 8-11 ---
    .long Default_Handler          | 8:  Privilege Violation
    .long Default_Handler          | 9:  Trace
    .long Default_Handler          | 10: A-line emulator
    .long Default_Handler          | 11: F-line emulator

    | --- Exception vectors 12-23: Reserved ---
    .rept 12
    .long Default_Handler
    .endr

    | --- Exception vectors 24-31: Interrupts ---
    .long Default_Handler          | 24: Spurious Interrupt
    .long Default_Handler          | 25: Level 1 autovector
    .long Default_Handler          | 26: Level 2 autovector
    .long Default_Handler          | 27: Level 3 autovector
    .long Default_Handler          | 28: Level 4 autovector
    .long Default_Handler          | 29: Level 5 autovector
    .long m68k_timer_isr           | 30: Level 6 autovector (Goldfish RTC)
    .long Default_Handler          | 31: Level 7 autovector (NMI)

    | --- Exception vectors 32-47: TRAP #0-#15 ---
    .long m68k_trap0_handler       | 32: TRAP #0 (PPAP syscall)
    .long m68k_trap1_handler       | 33: TRAP #1 (cooperative yield)
    .rept 14
    .long Default_Handler          | 34-47: TRAP #2 - TRAP #15
    .endr

    | --- Exception vectors 48-63: Reserved ---
    .rept 16
    .long Default_Handler
    .endr

    | --- Exception vectors 64-255: User interrupt vectors ---
    | Used by Goldfish PIC on QEMU virt machine
    .rept 192
    .long Default_Handler
    .endr


| =============================================================================
| Reset_Handler
|
| The CPU jumps here on power-on/reset.  SSP is already set to __stack_top
| by the hardware (loaded from vector 0) before this runs.
|
| Sequence:
|   1. Copy .data image from ROM/load region to RAM
|   2. Zero the .bss region
|   3. Call kmain() — must never return
| =============================================================================

    .section .text.reset_handler, "ax"
    .align 2
    .global Reset_Handler
Reset_Handler:
    | -----------------------------------------------------------------
    | Set SSP explicitly — QEMU's -kernel may not load initial SSP
    | from vector 0 the way real hardware does.
    | -----------------------------------------------------------------
    lea.l    __stack_top,%sp

    | -----------------------------------------------------------------
    | Copy .data: load region → RAM
    |   a0 = source (load address)
    |   a1 = destination start
    |   a2 = destination end
    | -----------------------------------------------------------------
    lea.l    __data_load,%a0
    lea.l    __data_start,%a1
    lea.l    __data_end,%a2
    bra.s    .Lcopy_check
.Lcopy_loop:
    move.l   (%a0)+,(%a1)+
.Lcopy_check:
    cmp.l    %a2,%a1
    blo.s    .Lcopy_loop

    | -----------------------------------------------------------------
    | Zero .bss
    |   a0 = start
    |   a1 = end
    | -----------------------------------------------------------------
    lea.l    __bss_start,%a0
    lea.l    __bss_end,%a1
    bra.s    .Lzero_check
.Lzero_loop:
    clr.l    (%a0)+
.Lzero_check:
    cmp.l    %a1,%a0
    blo.s    .Lzero_loop

    | -----------------------------------------------------------------
    | Enter kernel — kmain() must never return
    | -----------------------------------------------------------------
    jsr      kmain

    | Halt if kmain() returns unexpectedly
.Lhalt:
    stop     #0x2700             | halt with all IRQs masked
    bra.s    .Lhalt


| =============================================================================
| Default_Handler
|
| Catches all unhandled exceptions.
| Halts in a tight loop — attach a debugger to inspect PC/SR.
| =============================================================================

    .section .text, "ax"
    .align 2
    .global Default_Handler
Default_Handler:
    stop     #0x2700             | halt with all IRQs masked
    bra.s    Default_Handler
