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
 * FINAL LAYOUT (Step 8+)
 * =============================================================================
 * The SDK boot2 (first 256 bytes of FLASH_BOOT) initialises QSPI/XIP and
 * exits by reading {SP, entry} from XIP_BASE+0x100 = 0x10000100.
 * Our stage1.S is placed there: it sets VTOR=0x10001000, then loads SP and
 * Reset_Handler from the kernel vector table at 0x10001000 and branches.
 *
 *   0x10000000  Stage 1 (4KB):
 *               [0x10000000–0x100000FF]  Pico SDK boot2 (256 B, QSPI init)
 *               [0x10000100–0x10000FFF]  stage1.S (VTOR redirect to kernel)
 *   0x10001000  Kernel (64KB):           Vector table + .text + .rodata
 *   0x10011000  romfs (~16MB remainder): Populated by mkromfs (Phase 2+)
 *
 * =============================================================================
 * MEMORY REGIONS
 * =============================================================================
 *
 * Flash layout (XIP base = 0x10000000):
 *
 *   0x10000000  +-----------------------+  <- FLASH_BOOT  (4 KB)
 *               | SDK boot2 (256 B)     |     QSPI init, enables XIP
 *               | stage1 extension      |     Sets VTOR, jumps to kernel
 *   0x10001000  +-----------------------+  <- FLASH_KERNEL (64 KB)
 *               | Kernel .text/.rodata  |     Vector table at 0x10001000,
 *               | (vector table first)  |     then kernel code + data
 *   0x10011000  +-----------------------+  <- FLASH_ROMFS  (~16 MB remainder)
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
     * FLASH_BOOT: 4 KB — SDK boot2 (256 B) + stage1 extension */
    FLASH_BOOT   (rx)  : ORIGIN = 0x10000000, LENGTH = 4K
    FLASH_KERNEL (rx)  : ORIGIN = 0x10001000, LENGTH = 64K
    FLASH_ROMFS  (r)   : ORIGIN = 0x10011000, LENGTH = 16M - 0x11000

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
     * the QSPI controller for XIP, then exits by reading SP (word 0)
     * and entry (word 1) from 0x10000100 (start of .stage1 below).
     * ------------------------------------------------------------------ */
    .boot2 : {
        KEEP(*(.boot2))
    } > FLASH_BOOT


    /* ------------------------------------------------------------------
     * Stage 1 extension — placed immediately after boot2 at 0x10000100.
     *
     * The SDK boot2 exit stub reads:
     *   [0x10000100] = __stack_top    → sets MSP
     *   [0x10000104] = stage1_entry   → branches here (Thumb bit set)
     *
     * stage1_entry sets VTOR=0x10001000 and jumps to the kernel.
     * ------------------------------------------------------------------ */
    .stage1 : {
        KEEP(*(.stage1))
        . = ALIGN(4);
    } > FLASH_BOOT


    /* ------------------------------------------------------------------
     * Interrupt vector table — placed at the very start of FLASH_KERNEL
     * (= 0x10001000).  stage1_entry reads:
     *   [0x10001000] = __stack_top    → initial SP
     *   [0x10001004] = Reset_Handler  → entry point
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
     * romfs image — built by mkromfs, linked via objcopy'd romfs.o.
     * Contains the root filesystem (/, /etc, /bin, ...).
     * __romfs_start is passed to vfs_mount() as the image base address.
     * ------------------------------------------------------------------ */
    .romfs : {
        __romfs_start = .;
        KEEP(*(.romfs))
        . = ALIGN(4);
        __romfs_end = .;
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
     * RAM-resident functions (.ramfunc)
     * LMA: stored in flash immediately after .data image
     * VMA: copied to RAM_KERNEL at boot by startup.S
     *
     * Used for code that must run from SRAM (e.g., sram_bench for XIP
     * cache comparison).  Unlike .data, the section type is "ax"
     * (allocated + executable) so the assembler does not warn.
     * ------------------------------------------------------------------ */
    .ramfunc : {
        __ramfunc_start = .;
        *(.ramfunc*)
        . = ALIGN(4);
        __ramfunc_end = .;
    } > RAM_KERNEL AT> FLASH_KERNEL

    /* Flash address where .ramfunc image is stored (used by startup.S) */
    __ramfunc_load = LOADADDR(.ramfunc);


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
