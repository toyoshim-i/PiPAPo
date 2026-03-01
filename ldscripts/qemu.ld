/*
 * qemu.ld — Linker Script for QEMU mps2-an500 target
 *
 * Used by the ppap_qemu CMake target only (not for real RP2040 hardware).
 *
 * Machine: QEMU mps2-an500 (Cortex-M0+, same ISA as RP2040)
 *
 *   ROM   0x00000000  8 MB   — code and read-only data (no XIP, no boot2)
 *   RAM   0x20000000  512 KB — data, BSS, stack, ramfunc copies
 *
 * The CPU reads {SP, Reset_Handler} from [0x00000000] on reset (just like
 * a physical Cortex-M0+ board).  There is no boot ROM, no QSPI init, and
 * no stage1.S — the vector table sits directly at 0x00000000.
 *
 * Peripheral differences from RP2040:
 *   UART0: CMSDK UART at 0x40004000 (see src/drivers/uart_qemu.c)
 *   Clock: fixed emulated clock (no PLL, no XOSC)
 *   SysTick: present (ARM standard), but timing is emulation-speed
 */

MEMORY {
    ROM  (rx)  : ORIGIN = 0x00000000, LENGTH = 8M
    RAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
}

SECTIONS {

    /* ------------------------------------------------------------------
     * Interrupt vector table — first thing in ROM (read by CPU on reset).
     *   [0x00000000] = __stack_top    → initial SP
     *   [0x00000004] = Reset_Handler  → entry point
     * ------------------------------------------------------------------ */
    .vectors : {
        KEEP(*(.vectors))
        . = ALIGN(4);
    } > ROM


    /* ------------------------------------------------------------------
     * Kernel code and read-only data
     * ------------------------------------------------------------------ */
    .text : {
        *(.text.reset_handler)  /* Reset_Handler right after vectors */
        *(.text*)
        . = ALIGN(4);
    } > ROM

    .rodata : {
        *(.rodata*)
        . = ALIGN(4);
    } > ROM


    /* ------------------------------------------------------------------
     * romfs image — same content as on RP2040, placed in ROM.
     * __romfs_start is used by vfs_mount() as the image base address.
     * ------------------------------------------------------------------ */
    .romfs : {
        __romfs_start = .;
        KEEP(*(.romfs))
        . = ALIGN(4);
        __romfs_end = .;
    } > ROM


    /* ------------------------------------------------------------------
     * Initialized data — LMA in ROM, VMA in RAM (copied by Reset_Handler)
     * ------------------------------------------------------------------ */
    .data : {
        __data_start = .;
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT> ROM

    __data_load = LOADADDR(.data);


    /* ------------------------------------------------------------------
     * RAM-resident functions (.ramfunc) — same copy pattern as .data
     * ------------------------------------------------------------------ */
    .ramfunc : {
        __ramfunc_start = .;
        *(.ramfunc*)
        . = ALIGN(4);
        __ramfunc_end = .;
    } > RAM AT> ROM

    __ramfunc_load = LOADADDR(.ramfunc);


    /* ------------------------------------------------------------------
     * Zero-initialized data
     * ------------------------------------------------------------------ */
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM


    /* ------------------------------------------------------------------
     * Stack — 16 KB (generous for QEMU; RAM is plentiful)
     * ------------------------------------------------------------------ */
    .stack (NOLOAD) : {
        . = ALIGN(8);
        __stack_bottom = .;
        . += 16K;
        __stack_top = .;
    } > RAM


    /* Discard ARM unwind info (not needed) */
    /DISCARD/ : {
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
