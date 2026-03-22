/*
 * libc_riscv.ld — Linker script for musl PIE binaries (RISC-V)
 *
 * RISC-V PIC uses auipc (PC-relative) for all symbol references,
 * including writable statics.  Text and data must be loaded
 * contiguously in SRAM so PC-relative offsets are correct.
 *
 * Unlike the ARM version which separates text (XIP) from data (SRAM),
 * this script places everything in a single RW segment.  The kernel's
 * ELF loader allocates contiguous SRAM pages and copies the entire
 * image there.
 *
 * Built with -pie (Linux toolchain) to emit R_RISCV_RELATIVE
 * relocations (.rela.dyn) for GOT entries and function pointer tables.
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

    /* ── Executable segment ──────────────────────────────────────── */
    .init : {
        KEEP(*(.init))
    } :text

    .text : {
        *(.text*)
    } :text

    .fini : {
        KEEP(*(.fini))
    } :text

    .rodata : {
        *(.rodata*)
        *(.srodata*)
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

    /* ── Read-write segment ──────────────────────────────────────── */
    . = ALIGN(4);

    .got : {
        *(.got*)
    } :data

    .data : {
        *(.data*)
        *(.sdata*)
        *(.data.rel.ro*)
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
        *(.interp)
    }
}
