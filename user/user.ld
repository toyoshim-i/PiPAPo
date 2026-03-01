/*
 * user.ld — Linker script for PPAP user-space ELF binaries
 *
 * Binaries are linked at address 0.  For romfs XIP execution, the actual
 * runtime address is determined by where mkromfs places the file data in
 * flash.  The ELF loader (Phase 3 Step 3) handles the address mapping.
 *
 * Sections: .text (crt0 first) → .rodata → .data → .bss
 * Exports _end for future brk/sbrk heap base (Phase 3 Step 9).
 */

ENTRY(_start)

SECTIONS
{
    . = 0;

    .text : {
        *(.text.crt0)       /* crt0.S _start entry — must be first */
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

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
        *(.ARM.attributes)
        *(.ARM.exidx*)
        *(.comment)
        *(.note*)
    }
}
