/*
 * busybox.ld — Linker script for PPAP busybox (PIC, PIE, XIP from flash)
 *
 * Same model as user/user.ld: binary linked at address 0 with two
 * PT_LOAD segments.  The kernel's exec.c relocates GOT entries at
 * load time so text refs point into flash and data refs into SRAM.
 *
 * Built with -pie to emit R_ARM_RELATIVE relocations (.rel.dyn).
 * These tell exec.c which .data words contain addresses that need
 * adjustment — e.g. function-pointer arrays like applet_main[].
 *
 * Differences from user.ld:
 *   - No .text.crt0 placement (musl's crt1.o provides _start)
 *   - Handles .init/.fini sections from musl CRT
 *   - .rel.dyn in text segment (for PIE relocation at load time)
 *   - .data.rel.ro merged into data segment (relocated pointer tables)
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
        KEEP(*(.init))
    } :text

    .text : {
        *(.text*)
    } :text

    .fini : {
        KEEP(*(.fini))
    } :text

    /* With -fdata-sections, each const variable gets a named section
     * (.rodata.<name>).  String pools (.rodata.str*) and numeric
     * constant pools (.rodata.cst*) are always named regardless of
     * the flag.  All named subsections are pure constants — safe in
     * flash.  Only bare .rodata (from pre-compiled libgcc, which was
     * built without -fdata-sections) may contain jump tables with
     * R_ARM_RELATIVE entries and must stay in the writable data
     * segment. */
    .rodata.str : {
        *(.rodata.str*)
    } :text

    .rodata.cst : {
        *(.rodata.cst*)
    } :text

    .rodata.sub : {
        *(.rodata.*)
    } :text

    /* PIE relocation table — read by exec.c at load time */
    .rel.dyn : {
        __rel_dyn_start = .;
        *(.rel.dyn)
        *(.rel.data*)
        *(.rel.got*)
        *(.rel.rodata*)
        __rel_dyn_end = .;
    } :text

    /* Dynamic section (required by -pie linker) */
    .dynamic : {
        *(.dynamic)
    } :text

    .dynsym : {
        *(.dynsym)
    } :text

    .dynstr : {
        *(.dynstr)
    } :text

    .hash : {
        *(.hash)
        *(.gnu.hash)
    } :text

    /* ── Read-write segment (copied to SRAM by kernel) ──────────── */
    . = ALIGN(4);

    .got : {
        *(.got*)
    } :data

    /* Bare .rodata from pre-compiled libraries (libgcc) that may
     * contain jump tables with R_ARM_RELATIVE entries.  Must stay in
     * SRAM so exec.c can patch them.  Named subsections were already
     * consumed by .rodata.sub above. */
    .rodata : {
        *(.rodata)
    } :data

    .data : {
        *(.data*)
        *(.data.rel.ro*)
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
        *(.interp)
    }
}
