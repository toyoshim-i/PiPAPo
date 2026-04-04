/*
 * pico2rv.ld — Linker script for Pico 2 RISC-V mode (4 MB flash)
 *
 * Same flash and SRAM layout as pico2.ld (ARM mode).  Differences:
 *   - ENTRY(_start) instead of Reset_Handler
 *   - __kernel_entry instead of __kernel_vtor (RISC-V has no VTOR)
 *   - __global_pointer$ for RISC-V gp-relative addressing
 *   - No .ARM.exidx sections
 *   - No .nsc_veneer (TrustZone is ARM-only)
 *
 * Flash layout (XIP base = 0x10000000):
 *
 *   0x10000000  +-----------------------+  <- FLASH_BOOT  (4 KB)
 *               | stage1 code           |     Sets mtvec, jumps to kernel
 *               | picobin IMAGE_DEF     |     RP2350 boot ROM metadata
 *   0x10001000  +-----------------------+  <- FLASH_KERNEL (184 KB)
 *               | Kernel .text/.rodata  |     _trap_vector at 0x10001000,
 *               | (trap vector first)   |     then kernel code + data
 *   0x1002F000  +-----------------------+  <- FLASH_ROMFS  (~3.8 MB remainder)
 *               | romfs image           |     Populated by mkromfs
 *               +-----------------------+
 *
 * SRAM layout (520 KB total on RP2350):
 *   24 KB kernel, 464 KB pages (116 pages), 16 KB iobuf, 8 KB DMA
 */

ENTRY(_start)

MEMORY {
    /* External QSPI Flash — XIP mapped at 0x10000000
     * 4 MB total */
    FLASH_BOOT   (rx)  : ORIGIN = 0x10000000, LENGTH = 4K
    FLASH_KERNEL (rx)  : ORIGIN = 0x10001000, LENGTH = 184K
    FLASH_ROMFS  (r)   : ORIGIN = 0x1002F000, LENGTH = 4M - 0x2F000

    /* On-chip SRAM — 520KB total */
    RAM_KERNEL   (rwx) : ORIGIN = 0x20000000, LENGTH = 24K
    RAM_PAGES    (rwx) : ORIGIN = 0x20006000, LENGTH = 464K
    RAM_IOBUF    (rw)  : ORIGIN = 0x2007A000, LENGTH = 16K
    RAM_DMA      (rw)  : ORIGIN = 0x2007E000, LENGTH = 8K
}

/*
 * =============================================================================
 * SECTIONS
 * =============================================================================
 */

/* Linker symbol used by stage1.S to set mtvec */
__kernel_entry = ORIGIN(FLASH_KERNEL);
/* Boot ROM enters stage1 with the PICOBIN-provided SP.  Keep that at the
 * top of SRAM, matching the Pico SDK's RISC-V startup convention. */
__bootrom_stack_top = ORIGIN(RAM_DMA) + LENGTH(RAM_DMA);

SECTIONS {

    .stage1 : {
        KEEP(*(.stage1))
        . = ALIGN(4);
    } > FLASH_BOOT

    .embedded_block : {
        KEEP(*(.embedded_block))
        . = ALIGN(4);
    } > FLASH_BOOT

    .vectors : {
        KEEP(*(.vectors))
        . = ALIGN(4);
    } > FLASH_KERNEL

    .text : {
        *(.text.reset_handler)
        *(.text*)
        . = ALIGN(4);
    } > FLASH_KERNEL

    .rodata : {
        *(.rodata*)
        *(.srodata*)     /* RISC-V small read-only data (gp-relative) */
        . = ALIGN(4);
    } > FLASH_KERNEL

    __flash_kernel_end = .;

    .romfs : {
        __romfs_start = .;
        KEEP(*(.romfs))
        . = ALIGN(4);
        __romfs_end = .;
    } > FLASH_ROMFS

    .data : {
        __data_start = .;
        *(.data*)
        *(.sdata*)       /* RISC-V small initialized data (gp-relative) */
        . = ALIGN(4);
        __data_end = .;
    } > RAM_KERNEL AT> FLASH_KERNEL

    __data_load = LOADADDR(.data);

    /* Global pointer — placed in the middle of .sdata for efficient access.
     * RISC-V gp-relative loads use a 12-bit signed offset (±2 KB). */
    PROVIDE(__global_pointer$ = __data_start + 0x800);

    .ramfunc : {
        __ramfunc_start = .;
        *(.ramfunc*)
        . = ALIGN(4);
        __ramfunc_end = .;
    } > RAM_KERNEL AT> FLASH_KERNEL

    __ramfunc_load = LOADADDR(.ramfunc);

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(.sbss*)        /* RISC-V small uninitialized data (gp-relative) */
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM_KERNEL

    .stack (NOLOAD) : {
        . = ALIGN(8);
        __stack_bottom = .;
        . += 4K;
        __stack_top = .;
    } > RAM_KERNEL

    ASSERT((__stack_top <= ORIGIN(RAM_KERNEL) + LENGTH(RAM_KERNEL)),
           "ERROR: Kernel data + bss + stack overflow RAM_KERNEL (24KB)")

    __flash_kernel_used = (LOADADDR(.ramfunc) + SIZEOF(.ramfunc)) - ORIGIN(FLASH_KERNEL);
    __flash_kernel_free = LENGTH(FLASH_KERNEL) - __flash_kernel_used;
    ASSERT((LOADADDR(.ramfunc) + SIZEOF(.ramfunc) <= ORIGIN(FLASH_KERNEL) + LENGTH(FLASH_KERNEL)),
           "ERROR: Kernel text + data LMA overflows FLASH_KERNEL")

    .page_pool (NOLOAD) : {
        __page_pool_start = .;
        . += LENGTH(RAM_PAGES);
        __page_pool_end = .;
    } > RAM_PAGES

    .iobuf (NOLOAD) : {
        __iobuf_start = .;
        *(.iobuf .iobuf.*)
        . = ALIGN(4);
        __iobuf_alloc_end = .;
        . = __iobuf_start + LENGTH(RAM_IOBUF);
        __iobuf_end = .;
    } > RAM_IOBUF

    .dma_reserved (NOLOAD) : {
        __dma_start = .;
        . += LENGTH(RAM_DMA);
        __dma_end = .;
    } > RAM_DMA

    /DISCARD/ : {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.boot2)
    }
}
