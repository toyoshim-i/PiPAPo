/*
 * user.ld — Linker script for PPAP user-space ELF binaries (RISC-V)
 *
 * ePIC uses gp-relative addressing for all data access, including rodata.
 * Keep rodata and data in SRAM as contiguous PT_LOAD regions so the kernel
 * can copy both into one SRAM allocation and seed gp accordingly.
 *
 *   text   (R+X): .text                           (XIP)
 *   rodata (R)  : .rodata/.srodata + rela/dynsym (SRAM)
 *   data   (RW) : .data.rel.ro/.got/.data/.bss   (SRAM)
 */

ENTRY(_start)

/* Force ET_DYN so the linker preserves GOT entries for PIC relocation.
 * The kernel ELF loader patches GOT entries at load time. */
PHDRS
{
    text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    rodata PT_LOAD FLAGS(4); /* PF_R         */
    data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS
{
    . = 0;

    /* ── Executable segment (XIP) ───────────────────────────────── */
    .text : {
        *(.text.crt0)       /* crt0.S _start entry — must be first */
        *(.text*)
    } :text

    /* ── Read-only segment (SRAM, gp-accessible) ───────────────── */
    . = ALIGN(16);

    .rodata : {
        *(.rodata*)
        *(.srodata*)
    } :rodata

    .rela.dyn : {
        *(.rela.dyn)
        *(.rela.data*)
        *(.rela.got*)
        *(.rela.rodata*)
    } :rodata

    .dynamic : {
        *(.dynamic)
    } :rodata

    .dynsym : {
        *(.dynsym)
    } :rodata

    .dynstr : {
        *(.dynstr)
    } :rodata

    .hash : {
        *(.hash)
    } :rodata

    .gnu.hash : {
        *(.gnu.hash)
    } :rodata

    /* ── Read-write segment (SRAM, gp-accessible) ───────────────── */
    . = ALIGN(16);

    .data.rel.ro : {
        *(.data.rel.ro*)
    } :data

    .got : {
        *(.got*)
    } :data

    .data : {
        *(.data*)
        *(.sdata*)
    } :data

    .bss : {
        __bss_start = .;
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        __bss_end = .;
    } :data

    _end = .;

    /DISCARD/ : {
        *(.riscv.attributes)
        *(.comment)
        *(.note*)
    }
}
