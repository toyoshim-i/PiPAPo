/*
 * user.ld — Linker script for PPAP user-space ELF binaries (RISC-V)
 *
 * Same two-segment layout as the ARM version:
 *   text (R+X): .text + .rodata — executed directly from flash via XIP
 *   data (RW):  .got + .data + .bss — copied to an SRAM page by the kernel
 *
 * RISC-V-specific: includes .srodata/.sdata/.sbss for gp-relative access.
 */

ENTRY(_start)

/* Force ET_DYN so the linker preserves GOT entries for PIC relocation.
 * The kernel ELF loader patches GOT entries at load time. */
PHDRS
{
    text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS
{
    . = 0;

    /* ── Read-only executable segment (stays in flash via XIP) ──── */
    .text : {
        *(.text.crt0)       /* crt0.S _start entry — must be first */
        *(.text*)
    } :text

    .rodata : {
        *(.rodata*)
        *(.srodata*)
    } :text

    /* ── Read-write segment (copied to SRAM by kernel) ──────────── */
    . = ALIGN(4);

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
