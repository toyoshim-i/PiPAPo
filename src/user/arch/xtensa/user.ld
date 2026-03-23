/*
 * user.ld — Linker script for PPAP user-space ELF binaries (Xtensa)
 *
 * Two-segment layout:
 *   text (R+X): .text only — loaded into IRAM (word-access only)
 *   data (RW):  .rodata + .got + .data + .bss — loaded into DRAM (byte-accessible)
 *
 * ESP32-S3 IRAM only supports 32-bit word-aligned access.  String constants
 * and other read-only data MUST be in the data segment (DRAM) so that user
 * code can do byte loads (l8ui, l16ui).
 *
 * Xtensa-specific: L32R literal pools are in the text segment (IRAM).
 * They contain 32-bit pointers loaded via word-aligned reads — fine for IRAM.
 * Those pointers may reference DRAM addresses (for .rodata); the ELF loader
 * applies split relocations (text→IRAM base, data→DRAM base).
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

    /* ── Executable segment (IRAM — word-access only) ──────────── */
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

    /* ── Data segment (DRAM — byte-accessible) ─────────────────── */
    . = ALIGN(4);

    .rodata : {
        *(.rodata*)
    } :data

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
