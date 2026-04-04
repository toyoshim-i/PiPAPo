/*
 * user_xip.ld — Experimental Xtensa linker script for future XIP binaries
 *
 * This is the XT-2.5 packaging path, not the current default runtime path.
 * The goal is to separate "how we package immutable code/data for flash"
 * from the existing RAM-loaded bring-up path in user.ld.
 *
 * Layout:
 *   literal (RW): .literal*                    — relocation/literal support
 *   text (R+X):   .text* + .rodata*            — intended to stay in flash
 *   data (RW):    .got + .data + .bss          — intended for RAM
 *
 * Current Xtensa code generation still emits text relocations, so these
 * binaries are not yet used by the runtime loader.  They are built as
 * artifacts so the XIP path can evolve without destabilizing bring-up.
 */

ENTRY(_start)

/* Optional flash-window prelink base for fixed-address XIP experiments.
 * The default remains 0 so the generic relocatable packaging path keeps
 * its current behaviour unless the build passes __ppap_xip_flash_base. */
__ppap_xip_flash_base =
    DEFINED(__ppap_xip_flash_base) ? __ppap_xip_flash_base : 0;

PHDRS
{
    literal PT_LOAD FLAGS(6); /* PF_R | PF_W */
    text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS
{
    . = __ppap_xip_flash_base;

    /* ── RAM-backed literal / relocation support ──────────────── */
    .literal : {
        *(.literal.text.crt0)
        *(.literal*)
    } :literal

    . = ALIGN(4);

    /* ── Flash-oriented text segment ───────────────────────────── */
    .text : {
        *(.text.crt0)
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
