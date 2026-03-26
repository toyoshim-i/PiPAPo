/*
 * user_xip.ld — Experimental Xtensa linker script for future XIP binaries
 *
 * This is the XT-2.5 packaging path, not the current default runtime path.
 * The goal is to separate "how we package immutable code/data for flash"
 * from the existing RAM-loaded bring-up path in user.ld.
 *
 * Layout:
 *   text (R+X): .literal* + .text* + .rodata*  — intended to stay in flash
 *   data (RW):  .got + .data + .bss            — intended for RAM
 *
 * Current Xtensa code generation still emits text relocations, so these
 * binaries are not yet used by the runtime loader.  They are built as
 * artifacts so the XIP path can evolve without destabilizing bring-up.
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

    /* ── Flash-oriented text segment ───────────────────────────── */
    .text : {
        *(.literal.text.crt0)
        *(.text.crt0)
        *(.literal*)
        *(.text*)
        *(.rodata*)
    } :text

    /* ── RAM-backed mutable segment ────────────────────────────── */
    . = ALIGN(4);

    .got : {
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
        *(.xtensa.info)
        *(.comment)
        *(.note*)
    }
}
