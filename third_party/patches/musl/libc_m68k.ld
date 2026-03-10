/*
 * libc_m68k.ld — Linker script for musl PIE binaries (m68k, sep-data, XIP)
 *
 * Same model as src/user/arch/m68k/user.ld: binary linked at address 0 with
 * two PT_LOAD segments.  The kernel's exec.c handles GOT base (a5) setup
 * and data segment relocation.
 *
 * Built with -msep-data + -pie to emit R_68K_RELATIVE relocations
 * (.rela.dyn) so exec.c can fix up function-pointer arrays in .data.
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

    /* ── Read-only executable segment (XIP from romfs) ──── */
    .init : {
        KEEP(*(.init))
    } :text

    .text : {
        *(.text*)
    } :text

    .fini : {
        KEEP(*(.fini))
    } :text

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
    .rela.dyn : {
        __rela_dyn_start = .;
        *(.rela.dyn)
        *(.rela.data*)
        *(.rela.got*)
        *(.rela.rodata*)
        __rela_dyn_end = .;
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
        _GLOBAL_OFFSET_TABLE_ = .;
        *(.got*)
    } :data

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
        *(.comment)
        *(.note*)
        *(.interp)
    }
}
