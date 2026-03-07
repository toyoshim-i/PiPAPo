/*
 * qemu_m68k.ld — Linker Script for QEMU virt m68k target
 *
 * QEMU virt machine: RAM starts at 0x00000000 (128 MB default).
 * The 68000 reads {SSP, PC} from addresses 0x0 and 0x4 on reset.
 *
 * Memory layout:
 *   0x00000000  1 KB   Vector table (256 × 4 bytes)
 *   0x00000400  ...    Kernel code (.text)
 *   ...               .rodata
 *   ...               .data (initialized data)
 *   ...               .bss (zero-initialized)
 *   ...               Stack (16 KB, supervisor stack)
 *   ...               Page pool (for mm_init)
 *
 * Everything runs from RAM — no separate ROM region.
 * The kernel binary is loaded by QEMU's -kernel option.
 */

MEMORY {
    RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 16M
}

SECTIONS {

    /* Discard build-id and other notes that would shift .vectors */
    /DISCARD/ : { *(.note*) }

    /* ------------------------------------------------------------------
     * Vector table — first 1 KB of RAM (read by CPU on reset).
     *   [0x00000000] = __stack_top    → initial SSP
     *   [0x00000004] = Reset_Handler  → entry point
     * ------------------------------------------------------------------ */
    .vectors : {
        KEEP(*(.vectors))
        . = ALIGN(4);
    } > RAM


    /* ------------------------------------------------------------------
     * Kernel code
     * ------------------------------------------------------------------ */
    .text : {
        *(.text.reset_handler)
        *(.text*)
        . = ALIGN(4);
    } > RAM

    .rodata : {
        *(.rodata*)
        . = ALIGN(4);
    } > RAM


    /* ------------------------------------------------------------------
     * romfs image (future — placeholder)
     * ------------------------------------------------------------------ */
    .romfs : {
        __romfs_start = .;
        KEEP(*(.romfs))
        . = ALIGN(4);
        __romfs_end = .;
    } > RAM


    /* ------------------------------------------------------------------
     * Initialized data (already in RAM, no LMA/VMA split needed)
     *
     * __data_load == __data_start for RAM-only targets, so the
     * copy loop in boot.S is a no-op (src == dst, length 0).
     * ------------------------------------------------------------------ */
    .data : {
        __data_start = .;
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM

    __data_load = __data_start;


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
     * Supervisor stack — 16 KB
     * ------------------------------------------------------------------ */
    .stack (NOLOAD) : {
        . = ALIGN(4);
        __stack_bottom = .;
        . += 16K;
        __stack_top = .;
    } > RAM


    /* ------------------------------------------------------------------
     * Page pool — remainder of RAM available for page_alloc()
     * ------------------------------------------------------------------ */
    .pagepool (NOLOAD) : {
        . = ALIGN(4096);
        __page_pool_start = .;
        /* Extends to end of RAM — page_alloc uses __page_pool_start
         * and a configured PAGE_COUNT */
    } > RAM
}
