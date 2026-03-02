/*
 * busybox.ld — Linker script for PPAP busybox (PIC, XIP from flash)
 *
 * Same model as user/user.ld: binary linked at address 0 with two
 * PT_LOAD segments.  The kernel's exec.c relocates GOT entries at
 * load time so text refs point into flash and data refs into SRAM.
 *
 * Differences from user.ld:
 *   - No .text.crt0 placement (musl's crt1.o provides _start)
 *   - Handles .init/.fini sections from musl CRT
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

    /* ── Read-only executable segment (stays in flash via XIP) ──── */
    .init : {
        *(.init)
    } :text

    .text : {
        *(.text*)
    } :text

    .fini : {
        *(.fini)
    } :text

    .rodata : {
        *(.rodata*)
    } :text

    /* ── Read-write segment (copied to SRAM by kernel) ──────────── */
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
        *(.ARM.attributes)
        *(.ARM.exidx*)
        *(.comment)
        *(.note*)
    }
}