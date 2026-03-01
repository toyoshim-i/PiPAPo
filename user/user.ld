/*
 * user.ld — Linker script for PPAP user-space PIC ELF binaries
 *
 * Binaries are linked at address 0 and compiled with -fPIC.  Two PT_LOAD
 * segments are created:
 *
 *   text (R+X): .text + .rodata — executed directly from flash via XIP
 *   data (RW):  .got + .data + .bss — copied to an SRAM page by the kernel
 *
 * All data references go through the GOT (r9 = GOT base, set by the kernel
 * ELF loader).  The kernel patches GOT entries at load time so text refs
 * point into flash and data refs point into the SRAM page.
 *
 * Exports _end for future brk/sbrk heap base (Phase 3 Step 9).
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
    .text : {
        *(.text.crt0)       /* crt0.S _start entry — must be first */
        *(.text*)
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
