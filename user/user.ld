/*
 * user.ld — Linker script for PPAP user-space ELF binaries
 *
 * Binaries are linked at USER_CODE_BASE (0x20020000) in SRAM.  The ELF
 * loader copies the code segment from romfs flash to this SRAM address
 * at exec time.  Absolute addresses in literal pools are correct because
 * the runtime address matches the link address.
 *
 * Sections: .text (crt0 first) → .rodata → .data → .bss
 * Exports _end for future brk/sbrk heap base (Phase 3 Step 9).
 */

ENTRY(_start)

SECTIONS
{
    . = 0x20020000;

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
