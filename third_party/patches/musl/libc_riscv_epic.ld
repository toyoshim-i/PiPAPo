/*
 * libc_riscv_epic.ld — Linker script for ePIC musl PIE binaries (RISC-V)
 *
 * ePIC uses GP-relative addressing for ALL data access including .rodata.
 * With XIP, text executes from romfs but rodata and data must be in SRAM
 * so GP can reach both.
 *
 * Layout:
 *   PT_LOAD  R+X  : .text                          (XIP from romfs)
 *   PT_LOAD  R    : .rodata, .rela.dyn, .dynamic   (SRAM, GP-accessible)
 *   PT_LOAD  R+W  : .data, .sdata, .bss            (SRAM, GP-accessible)
 *
 * The R and RW segments are at contiguous virtual addresses so the kernel
 * allocates them in one SRAM block.  Future PMP can mark rodata read-only.
 */

ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    rodata PT_LOAD FLAGS(4);   /* PF_R         */
    data   PT_LOAD FLAGS(6);   /* PF_R | PF_W  */
}

SECTIONS
{
    . = 0;

    /* ── Executable segment (XIP) ────────────────────────────────── */
    .text : {
        KEEP(*(.init))
        *(.text*)
        KEEP(*(.fini))
    } :text

    /* ── Read-only data segment (SRAM, GP-accessible) ────────────── */
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

    /* ── Read-write data segment (SRAM, GP-accessible) ───────────── */
    . = ALIGN(16);

    .data.rel.ro : {
        *(.data.rel.ro*)
    } :data

    .got : {
        *(.got*)
    } :data

    .sdata : {
        *(.sdata*)
    } :data

    .data : {
        *(.data*)
    } :data

    .sbss : {
        *(.sbss*)
    } :data

    .bss : {
        *(.bss*)
        *(COMMON)
    } :data

    _end = .;

    /DISCARD/ : {
        *(.riscv.attributes)
        *(.comment)
        *(.note*)
        *(.interp)
    }
}
