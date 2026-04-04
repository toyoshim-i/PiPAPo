/*
 * r68k.ld — Linker script for Human68k R-format (flat binary) programs
 *
 * Produces a position-independent flat binary with no headers.
 * The kernel loads the entire file at PMB base + 0x100.
 * Entry point is at the start of the binary (offset 0).
 *
 * Use with: m68k-elf-gcc -nostdlib -T r68k.ld -o prog.elf prog.o
 * Then:     m68k-elf-objcopy -O binary prog.elf prog.r
 */

ENTRY(_start)

SECTIONS
{
    . = 0;

    .text : {
        *(.text*)
        *(.rodata*)
    }

    . = ALIGN(4);

    .data : {
        *(.data*)
    }

    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.ARM.*)
    }
}
