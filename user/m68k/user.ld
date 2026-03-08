/*
 * user.ld — Linker script for PPAP m68k user-space ELF binaries
 *
 * Static (non-PIC) binaries linked at address 0.  One or two PT_LOAD
 * segments:
 *   text (R+X): .text + .rodata — accessed via XIP from romfs in RAM
 *   data (RW):  .got + .data + .bss — copied to SRAM page by kernel
 *               (only present if the binary has writable data)
 *
 * The kernel's ELF loader (exec.c) maps text for XIP and copies the
 * data segment to an allocated page.  GOT base is set in a5 by the
 * kernel for PIC binaries.
 */

ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS
{
    . = 0;

    /* ── Read-only executable segment (XIP from romfs) ──────────── */
    .text : {
        *(.text*)
    } :text

    .rodata : {
        *(.rodata*)
    } :text

    /* ── Read-write segment (copied to SRAM by kernel) ──────────── */
    . = ALIGN(4);

    .got : {
        _GLOBAL_OFFSET_TABLE_ = .;
        *(.got*)
    } :data

    .data : {
        *(.data*)
    } :data

    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    } :data

    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
    }
}
