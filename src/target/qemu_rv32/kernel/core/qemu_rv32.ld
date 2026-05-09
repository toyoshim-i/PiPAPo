/*
 * qemu_rv32.ld — Linker script for QEMU RISC-V virt machine
 *
 * QEMU virt memory map:
 *   ROM/Flash:  0x20000000 (8 MB) — kernel loaded here by -kernel
 *   RAM:        0x80000000 (128 MB, we use 512 KB)
 *
 * Kernel code runs from ROM (loaded by QEMU's -kernel option).
 * Data/BSS/stack in RAM at 0x80000000.
 */

ENTRY(_start)

MEMORY {
    ROM    (rx)  : ORIGIN = 0x80000000, LENGTH = 8M
    RAM    (rwx) : ORIGIN = 0x80800000, LENGTH = 1M
}

/* Trap vector at start of ROM */
__kernel_entry = ORIGIN(ROM);

SECTIONS {
    /* _start must be at ORIGIN(ROM) — QEMU jumps here on reset.
     * Trap vector follows immediately after _start's initial setup. */
    .text : {
        *(.text.reset_handler)
        KEEP(*(.vectors))
        *(.text*)
        . = ALIGN(4);
    } > ROM

    .rodata : {
        *(.rodata*)
        *(.srodata*)
        . = ALIGN(4);
    } > ROM

    __flash_kernel_end = .;

    .romfs : {
        __romfs_start = .;
        KEEP(*(.romfs))
        . = ALIGN(4);
        __romfs_end = .;
    } > ROM

    .data : {
        __data_start = .;
        *(.data*)
        *(.sdata*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT> ROM

    __data_load = LOADADDR(.data);

    PROVIDE(__global_pointer$ = __data_start + 0x800);

    .ramfunc : {
        __ramfunc_start = .;
        *(.ramfunc*)
        . = ALIGN(4);
        __ramfunc_end = .;
    } > RAM AT> ROM

    __ramfunc_load = LOADADDR(.ramfunc);

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM

    /* Per-process kernel stacks.  Slot 0 = 128 B (idle), slots 1..7
     * = 4 KB each.  Total 128 + 7 * 4096 = 28800 bytes.  Must match
     * PROC_KSTACK_IDLE_SIZE / PROC_KSTACK_SIZE / PROC_MAX in
     * src/kernel/common/config.h and this target's compile definitions. */
    .kstack (NOLOAD) : {
        . = ALIGN(8);
        __kstack_region_base = .;
        . += 28800;
        __kstack_region_end = .;
    } > RAM

    .stack (NOLOAD) : {
        . = ALIGN(8);
        __stack_bottom = .;
        . += 16K;
        __stack_top = .;
    } > RAM

    .page_pool (NOLOAD) : {
        . = ALIGN(4096);
        __page_pool_start = .;
        . = ORIGIN(RAM) + LENGTH(RAM) - 16K;
        __page_pool_end = .;
    } > RAM

    /* Sanity checks */
    ASSERT((__stack_top <= __page_pool_start),
           "ERROR: Kernel data + kstacks + stack overflows into page pool")
    ASSERT((__stack_top <= ORIGIN(RAM) + LENGTH(RAM)),
           "ERROR: Kernel data + bss + stack overflows RAM")

    /DISCARD/ : {
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
