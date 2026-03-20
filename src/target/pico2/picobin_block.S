@ =============================================================================
@ picobin_block.S — RP2350 image definition block
@
@ The RP2350 boot ROM searches for a PICOBIN block in the first 4 KB of flash
@ to determine image type, CPU, security world, and entry point.  Without this
@ block the ROM will not boot our image.
@
@ This block declares: ARM Secure executable, RP2350 chip.  An explicit
@ ENTRY_POINT item directs the ROM to stage1_entry, which sets VTOR to the
@ kernel vector table at FLASH_KERNEL and jumps to the kernel reset handler.
@
@ The block is placed in section .embedded_block, which the linker script puts
@ inside FLASH_BOOT (first 4 KB).
@ =============================================================================

.syntax unified
.cpu cortex-m33
.thumb

@ PICOBIN constants (from pico-sdk boot/picobin.h)
.equ PICOBIN_BLOCK_MARKER_START,        0xffffded3
.equ PICOBIN_BLOCK_MARKER_END,          0xab123579
.equ PICOBIN_BLOCK_ITEM_1BS_IMAGE_TYPE, 0x42
.equ PICOBIN_BLOCK_ITEM_1BS_ENTRY_POINT, 0x44
.equ PICOBIN_BLOCK_ITEM_2BS_LAST,       0xff     @ 0x80 | 0x7f

@ IMAGE_TYPE value for: EXE (1<<0) | Security=S (2<<4) | CPU=ARM (0<<8) | Chip=RP2350 (1<<12)
.equ IMAGE_TYPE_VALUE,              0x1021

.section .embedded_block, "a"
.p2align 2
.global __embedded_block
__embedded_block:
    .word PICOBIN_BLOCK_MARKER_START

    @ IMAGE_TYPE item: declares ARM Secure EXE on RP2350
    .byte PICOBIN_BLOCK_ITEM_1BS_IMAGE_TYPE
    .byte 0x01                          @ item size: 1 word
    .hword IMAGE_TYPE_VALUE             @ 0x1021

    @ ENTRY_POINT item: explicit entry so the ROM doesn't rely on vector table
    @ position relative to the IMAGE_DEF block.  Points to stage1_entry which
    @ sets VTOR to __kernel_vtor and jumps to the kernel reset handler.
    .byte PICOBIN_BLOCK_ITEM_1BS_ENTRY_POINT
    .byte 0x03                          @ item size: 3 words to next item
    .byte 0                             @ pad
    .byte 0                             @ pad
    .word stage1_entry                  @ entry point (Thumb address)
    .word __stack_top                   @ initial stack pointer

    @ LAST item: terminates the block, with self-referencing loop offset
    .byte PICOBIN_BLOCK_ITEM_2BS_LAST
    .hword (__embedded_block_end - __embedded_block - 16) / 4
    .byte 0
    .word 0                             @ loop offset = 0 (single block loop, points to self)

    .word PICOBIN_BLOCK_MARKER_END
__embedded_block_end:
