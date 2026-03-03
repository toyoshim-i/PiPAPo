/*
 * pico1.ld — Linker script for official Raspberry Pi Pico (2 MB flash)
 *
 * Identical to pico1calc.ld except FLASH_ROMFS is limited to ~1.9 MB
 * (the Pico has 2 MB flash vs PicoCalc's 16 MB).
 *
 * Flash layout (XIP base = 0x10000000):
 *
 *   0x10000000  +-----------------------+  <- FLASH_BOOT  (4 KB)
 *               | SDK boot2 (256 B)     |     QSPI init, enables XIP
 *               | stage1 extension      |     Sets VTOR, jumps to kernel
 *   0x10001000  +-----------------------+  <- FLASH_KERNEL (64 KB)
 *               | Kernel .text/.rodata  |     Vector table at 0x10001000,
 *               | (vector table first)  |     then kernel code + data
 *   0x10011000  +-----------------------+  <- FLASH_ROMFS  (~1.9 MB remainder)
 *               | romfs image           |     Populated by mkromfs (Phase 2+)
 *               +-----------------------+
 *
 * SRAM layout: identical to pico1calc (264 KB total).
 */

MEMORY {
    /* External QSPI Flash — XIP mapped at 0x10000000
     * 2 MB total (vs 16 MB on PicoCalc) */
    FLASH_BOOT   (rx)  : ORIGIN = 0x10000000, LENGTH = 4K
    FLASH_KERNEL (rx)  : ORIGIN = 0x10001000, LENGTH = 64K
    FLASH_ROMFS  (r)   : ORIGIN = 0x10011000, LENGTH = 2M - 0x11000

    /* On-chip SRAM — 264KB total */
    RAM_KERNEL   (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
    RAM_PAGES    (rwx) : ORIGIN = 0x20005000, LENGTH = 204K
    RAM_IOBUF    (rw)  : ORIGIN = 0x20038000, LENGTH = 24K
    RAM_DMA      (rw)  : ORIGIN = 0x2003E000, LENGTH = 16K
}

/*
 * =============================================================================
 * SECTIONS — identical to pico1calc.ld
 * =============================================================================
 */

SECTIONS {

    .boot2 : {
        KEEP(*(.boot2))
    } > FLASH_BOOT

    .stage1 : {
        KEEP(*(.stage1))
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
        . = ALIGN(4);
        __data_end = .;
    } > RAM_KERNEL AT> FLASH_KERNEL

    __data_load = LOADADDR(.data);

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
           "ERROR: Kernel data + bss + stack overflow RAM_KERNEL (20KB)")

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
    }
}
