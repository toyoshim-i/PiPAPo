/*
 * stage1.S — IBM PC floppy boot sector (sector 0, 512 bytes)
 *
 * BIOS loads this to 0x0000:0x7C00 and jumps here.
 * Loads 8 sectors (stage2, 4 KB) from floppy to 0x0000:0xC000,
 * then jumps to stage2 entry at 0x0000:0xC000.
 *
 * Includes a minimal BPB (BIOS Parameter Block) for 720 KB floppy
 * so that BIOSes recognise the floppy format.
 */

  .code16
  .section .text, "ax"
  .global _start

_start:
  jmp   .Lstart
  nop

  /* ── BPB (BIOS Parameter Block) for 720 KB 3.5" DD floppy ─────────── */
  /* offset 3 */
  .ascii "PPAP    "           /* OEM name (8 bytes) */
  .word  512                  /* bytes per sector */
  .byte  2                    /* sectors per cluster */
  .word  1                    /* reserved sectors */
  .byte  2                    /* number of FATs (unused, but standard) */
  .word  112                  /* root directory entries */
  .word  1440                 /* total sectors (720 KB) */
  .byte  0xF9                 /* media descriptor (3.5" 720 KB DD) */
  .word  3                    /* sectors per FAT (unused) */
  .word  9                    /* sectors per track */
  .word  2                    /* number of heads */
  .long  0                    /* hidden sectors */
  .long  0                    /* total sectors (large, 0 for <32MB) */
  .byte  0                    /* drive number (filled by BIOS) */
  .byte  0                    /* reserved */
  .byte  0x29                 /* extended boot signature */
  .long  0x50504150           /* volume serial number ('PPAP') */
  .ascii "PPAP BOOT  "       /* volume label (11 bytes) */
  .ascii "UFS     "           /* filesystem type (8 bytes) */

  /* ── Boot code (offset 62) ────────────────────────────────────────── */

  .include "stage1_common.inc"

  /* Pad to 510 bytes and add boot signature */
  .org  510
  .word 0xAA55
