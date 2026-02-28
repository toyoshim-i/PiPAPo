/*
 * ppap.ld — PicoPiAndPortable Linker Script
 *
 * Defines the flash and SRAM memory layout for the RP2040 target.
 * Based on design specification Section 2.2 (Flash Layout) and
 * Section 2.3 (SRAM Memory Map).
 *
 * Flash is XIP-mapped at 0x10000000 on the RP2040.
 * All flash regions are execute-in-place; no code copying to SRAM.
 *
 * =============================================================================
 * INTERIM LAYOUT (Steps 4–7)
 * =============================================================================
 * The RP2040 boot ROM, after executing boot2, reads the application vector
 * table from XIP_BASE + 0x100 (= 0x10000100).  While we still use the Pico
 * SDK's 256-byte boot2, the kernel must start there.
 *
 * Step 8 replaces boot2 with our own 4 KB Stage 1 that sets VTOR and jumps
 * to 0x10001000, restoring the final layout described in the design spec:
 *
 *   FINAL (Step 8+):            INTERIM (now):
 *   0x10000000  Stage 1 (4KB)   0x10000000  boot2 (256 B)
 *   0x10001000  Kernel           0x10000100  Kernel (vector table here)
 *   0x10011000  romfs            0x10010100  romfs
 *
 * =============================================================================
 * MEMORY REGIONS
 * =============================================================================
 *
 * Flash layout (XIP base = 0x10000000):
 *
 *   0x10000000  +-----------------------+  <- FLASH_BOOT  (256 B)
 *               | Pico SDK boot2        |     QSPI init, enables XIP
 *   0x10000100  +-----------------------+  <- FLASH_KERNEL (64KB)
 *               | Kernel .text/.rodata  |     Vector table at 0x10000100,
 *               | (vector table first)  |     then kernel code + data
 *   0x10010100  +-----------------------+  <- FLASH_ROMFS  (~16MB remainder)
 *               | romfs image           |     Populated by mkromfs (Phase 2+)
 *               +-----------------------+
 *
 * SRAM layout (total 264KB):
 *
 *   0x20000000  +-----------------------+  <- RAM_KERNEL  (16KB)
 *               | Kernel .data/.bss     |     Kernel globals, initial stack
 *               | Stack (grows down)    |
 *   0x20004000  +-----------------------+  <- RAM_PAGES   (208KB = 52 × 4KB)
 *               | Page pool             |     User process pages (Phase 1+)
 *   0x20038000  +-----------------------+  <- RAM_IOBUF   (24KB)
 *               | I/O buffer            |     SD card DMA, FS metadata cache
 *   0x2003E000  +-----------------------+  <- RAM_DMA     (16KB)
 *               | DMA / PIO / Core 1    |     DMA buffers, PIO programs,
 *               | stack / IRQ stack     |     Core 1 stack, IRQ stack
 *   0x20042000  +-----------------------+  <- end of SRAM (264KB)
 */

MEMORY {
    /* External QSPI Flash — XIP mapped at 0x10000000
     * FLASH_BOOT: 256 B for Pico SDK boot2 (Step 8 expands this to 4KB) */
    FLASH_BOOT   (rx)  : ORIGIN = 0x10000000, LENGTH = 256
    FLASH_KERNEL (rx)  : ORIGIN = 0x10000100, LENGTH = 64K
    FLASH_ROMFS  (r)   : ORIGIN = 0x10010100, LENGTH = 16M - 0x10100

    /* On-chip SRAM — 264KB total */
    RAM_KERNEL   (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
    RAM_PAGES    (rwx) : ORIGIN = 0x20004000, LENGTH = 208K
    RAM_IOBUF    (rw)  : ORIGIN = 0x20038000, LENGTH = 24K
    RAM_DMA      (rw)  : ORIGIN = 0x2003E000, LENGTH = 16K
}

/*
 * =============================================================================
 * SECTIONS
 * =============================================================================
 */

SECTIONS {

    /* ------------------------------------------------------------------
     * Pico SDK boot2 — exactly 256 bytes at flash offset 0x000.
     *
     * The RP2040 boot ROM copies these 256 bytes to SRAM, verifies the
     * CRC32 in the last 4 bytes, and executes them.  Boot2 configures
     * the QSPI controller for XIP, then returns to the boot ROM, which
     * reads SP (word 0) and Reset_Handler (word 1) from 0x10000100 and
     * branches to Reset_Handler.
     *
     * Step 8 will replace bs2_default_library with our own stage1.S
     * and expand FLASH_BOOT to 4 KB.
     * ------------------------------------------------------------------ */
    .boot2 : {
        KEEP(*(.boot2))
    } > FLASH_BOOT


    /* ------------------------------------------------------------------
     * Interrupt vector table — placed at the very start of FLASH_KERNEL
     * (= 0x10000100).  The boot ROM reads:
     *   [0x10000100] = __stack_top    → initial SP
     *   [0x10000104] = Reset_Handler  → entry point
     * ------------------------------------------------------------------ */
    .vectors : {
        KEEP(*(.vectors))
        . = ALIGN(4);
    } > FLASH_KERNEL


    /* ------------------------------------------------------------------
     * Kernel code and read-only data — XIP executed from flash.
     * No copy to SRAM; only stack and heap reside in RAM.
     * ------------------------------------------------------------------ */
    .text : {
        *(.text.reset_handler)  /* Reset handler immediately after vectors */
        *(.text*)
        . = ALIGN(4);
    } > FLASH_KERNEL

    .rodata : {
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH_KERNEL

    /* Record where flash-resident kernel content ends */
    __flash_kernel_end = .;


    /* ------------------------------------------------------------------
     * romfs placeholder — populated by mkromfs in Phase 2.
     * Marked NOLOAD so the linker does not try to fill it.
     * ------------------------------------------------------------------ */
    .romfs (NOLOAD) : {
        __romfs_start = .;
    } > FLASH_ROMFS


    /* ------------------------------------------------------------------
     * Initialized data (.data)
     * LMA (load address): stored in flash right after .rodata
     * VMA (run address):  copied to RAM_KERNEL at boot by startup.S
     * ------------------------------------------------------------------ */
    .data : {
        __data_start = .;
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM_KERNEL AT> FLASH_KERNEL

    /* Flash address where .data image is stored (used by startup.S) */
    __data_load = LOADADDR(.data);


    /* ------------------------------------------------------------------
     * Zero-initialized data (.bss)
     * Zeroed by startup.S before calling kmain().
     * ------------------------------------------------------------------ */
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM_KERNEL


    /* ------------------------------------------------------------------
     * Kernel stack — 4KB, grows downward from __stack_top.
     * __stack_top is loaded into SP by the vector table entry.
     * ------------------------------------------------------------------ */
    .stack (NOLOAD) : {
        . = ALIGN(8);
        __stack_bottom = .;
        . += 4K;
        __stack_top = .;
    } > RAM_KERNEL

    /* Sanity check: kernel data + bss + stack must fit in 16KB */
    ASSERT((__stack_top <= ORIGIN(RAM_KERNEL) + LENGTH(RAM_KERNEL)),
           "ERROR: Kernel data + bss + stack overflow RAM_KERNEL (16KB)")


    /* ------------------------------------------------------------------
     * Page pool — managed by the memory manager (Phase 1+).
     * 52 pages × 4KB = 208KB.  Symbols exposed for kernel use.
     * ------------------------------------------------------------------ */
    .page_pool (NOLOAD) : {
        __page_pool_start = .;
        . += LENGTH(RAM_PAGES);
        __page_pool_end = .;
    } > RAM_PAGES


    /* ------------------------------------------------------------------
     * I/O buffer — SD card DMA transfers and FS metadata cache (Phase 4+).
     * ------------------------------------------------------------------ */
    .iobuf (NOLOAD) : {
        __iobuf_start = .;
        . += LENGTH(RAM_IOBUF);
        __iobuf_end = .;
    } > RAM_IOBUF


    /* ------------------------------------------------------------------
     * DMA / PIO / Core 1 — reserved for hardware use (Phase 1+).
     * ------------------------------------------------------------------ */
    .dma_reserved (NOLOAD) : {
        __dma_start = .;
        . += LENGTH(RAM_DMA);
        __dma_end = .;
    } > RAM_DMA


    /* Discard C++ exception tables and ARM unwind info (not needed) */
    /DISCARD/ : {
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
