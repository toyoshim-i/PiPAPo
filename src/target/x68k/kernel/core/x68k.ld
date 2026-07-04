/*
 * x68k.ld — Linker Script for X68000 target
 *
 * X68000 main RAM: 0x000000 – 0x0FFFFF (1 MB standard)
 *                  0x000000 – 0xBFFFFF (up to 12 MB expanded)
 *
 * The kernel binary is loaded by stage2 from the boot floppy to 0x006000.
 * Stage2 copies the .vectors section (1 KB) to 0x000000 (CPU vector table),
 * then patches vector 47 (TRAP #15) back to the IPL IOCS handler,
 * then jumps to Reset_Handler at 0x006400.
 *
 * Memory layout (after stage2 hands off to kernel):
 *   0x000000 – 0x0003FF   1 KB  Active vector table (copied from .vectors by stage2)
 *   0x000400 – 0x005FFF  22.75 KB  Freed boot memory → page pool after boot
 *   0x006000 – 0x0063FF   1 KB  Kernel .vectors in binary (reused as stack headroom)
 *   0x006400 – ...       ...   Kernel .text, .rodata, .data, .bss
 *   ...       – ...       ...   Fixed per-process kernel stacks
 *   ...       – 0xBFFFFF  ...   Page pool (user process pages)
 *
 * Supervisor stack: grows downward from __stack_top = 0x006400 into the
 * freed boot memory region (0x000400–0x005FFF = 22.75 KB).
 * No .stack section in the binary — the stack lives in free RAM.
 *
 * Rootfs is the 44bsd UFS on the boot floppy, mounted live via iocs_blk
 * ("fd0").  No embedded romfs and no in-RAM rootfs image.
 */

MEMORY {
    RAM (rwx) : ORIGIN = 0x006000, LENGTH = 0xBFA000
}

SECTIONS {

    /* Discard build-id, notes, and embedded romfs (rootfs lives on the
     * boot floppy as a 44bsd UFS, not linked into the kernel binary). */
    /DISCARD/ : { *(.note*) *(.romfs) }

    /* main.c gates embedded-romfs mount on __romfs_start != __romfs_end;
     * both zero here so it falls through to target_mount_rootfs. */
    __romfs_start = 0;
    __romfs_end   = 0;

    /* ------------------------------------------------------------------
     * Vector table — 1 KB at 0x006000 in the kernel binary.
     * Stage2 copies this to 0x000000 before jumping to Reset_Handler.
     * After the copy, this 1 KB region is reclaimed as supervisor stack
     * headroom (stack grows downward from __stack_top = 0x006400).
     * ------------------------------------------------------------------ */
    .vectors : {
        KEEP(*(.vectors))
        . = ALIGN(4);
    } > RAM

    /* __stack_top is the address immediately after .vectors = 0x006400.
     * The 68000 supervisor stack grows downward from here into the freed
     * boot memory (0x000400–0x005FFF = 22.75 KB available).             */
    __stack_top = .;

    /* ------------------------------------------------------------------
     * Kernel code — Reset_Handler lands here at 0x006400
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
     * Initialized data (RAM-only target: load == run address)
     *
     * __data_load == __data_start so the copy loop in boot.S is a no-op.
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
     * Per-process kernel stacks.  Slot 0 = 128 B (idle), slots 1..7
     * = 4 KB each.  Total 128 + 7 * 4096 = 28800 bytes.  Must match
     * PROC_KSTACK_IDLE_SIZE / PROC_KSTACK_SIZE / PROC_MAX in config.h and
     * this target's compile definitions (PROC_KSTACK_SIZE=4096).  The
     * shared m68k 2 KB default overruns on x68k: IOCS ROM calls run on
     * the process kstack and the ROM's interrupt frames nest on it under
     * the IPL=0 IOCS guard.  See src/target/x68k/CMakeLists.txt.
     * ------------------------------------------------------------------ */
    .kstack (NOLOAD) : {
        . = ALIGN(4);
        __kstack_region_base = .;
        . += 28800;
        __kstack_region_end = .;
    } > RAM

    /* ------------------------------------------------------------------
     * Page pool — remainder of RAM available for page_alloc().
     * On a 1 MB machine the pool runs to 0x0FFFFF (~820 KB depending on
     * kernel size).  On expanded hardware it continues to 0xBFFFFF.
     * The freed early-stage area (0x000400–0x005FFF) is registered as
     * additional pages at runtime in target_late_init().
     * ------------------------------------------------------------------ */
    .pagepool (NOLOAD) : {
        . = ALIGN(4096);
        __page_pool_start = .;
        /* Extends to end of physical RAM — detected at runtime */
    } > RAM

    /* Sanity checks */
    ASSERT((__stack_top <= __page_pool_start),
           "ERROR: Kernel stack overflows into page pool")
    ASSERT((__kstack_region_end <= __page_pool_start),
           "ERROR: Per-process kernel stacks overflow into page pool")
    ASSERT((__page_pool_start <= ORIGIN(RAM) + LENGTH(RAM)),
           "ERROR: Kernel image too large — page pool outside RAM")
}
