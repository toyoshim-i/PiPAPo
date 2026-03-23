/*
 * user.ld — Linker script for PPAP user-space ELF binaries (Xtensa)
 *
 * Same two-segment layout as ARM and RISC-V versions:
 *   text (R+X): .text + .rodata — loaded from romfs
 *   data (RW):  .got + .data + .bss — copied to an SRAM page by the kernel
 *
 * Xtensa-specific: includes .literal sections (used for L32R instruction
 * constant pools).
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

    /* ── Read-only executable segment ──────────────────────────── */
    .text : {
        /* L32R literal pools MUST precede the code that references them.
         * L32R uses a negative PC-relative offset (up to -256KB), so
         * literals placed after code are unreachable → linker generates
         * bad offsets → user code loads garbage addresses. */
        *(.literal.text.crt0)   /* crt0.S literals */
        *(.text.crt0)           /* crt0.S _start entry */
        *(.literal*)            /* all other literal pools */
        *(.text*)               /* all other code */
    } :text

    .rodata : {
        *(.rodata*)
    } :text

    /* ── Read-write segment (copied to SRAM by kernel) ─────────── */
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
