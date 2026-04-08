/*
 * stage1.S — IBM PC boot sector (sector 0, 512 bytes)
 *
 * BIOS loads this to 0x0000:0x7C00 and jumps here.
 * Loads 8 sectors (stage2, 4 KB) from floppy to 0x0000:0xC000,
 * then jumps to stage2 entry at 0x0000:0xC000.
 *
 * Includes a minimal BPB (BIOS Parameter Block) for 1.44 MB floppy
 * so that BIOSes recognise the floppy format.
 */

  .code16
  .section .text, "ax"
  .global _start

_start:
  jmp   .Lstart
  nop

  /* ── BPB (BIOS Parameter Block) for 1.44 MB 3.5" floppy ──────────── */
  /* offset 3 */
  .ascii "PPAP    "           /* OEM name (8 bytes) */
  .word  512                  /* bytes per sector */
  .byte  1                    /* sectors per cluster */
  .word  1                    /* reserved sectors */
  .byte  2                    /* number of FATs (unused, but standard) */
  .word  224                  /* root directory entries */
  .word  2880                 /* total sectors (1.44 MB) */
  .byte  0xF0                 /* media descriptor (3.5" 1.44 MB) */
  .word  9                    /* sectors per FAT (unused) */
  .word  18                   /* sectors per track */
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

.Lstart:
  cli
  xorw  %ax, %ax
  movw  %ax, %ds
  movw  %ax, %es
  movw  %ax, %ss
  movw  $0x7C00, %sp
  sti

  /* Save boot drive number */
  movb  %dl, drive_no

  /* Print "Pi" — stage1 portion of the progressive banner.
   * print_char writes to both BIOS teletype (VGA) and COM1. */
  movb  $'P', %al
  call  print_char
  movb  $'i', %al
  call  print_char

  /* Load 8 sectors (stage2) from floppy sectors 1-8 to 0x0000:0xC000 */
  movw  $0xC000, %bx       /* ES:BX = destination */
  movb  $0x02, %ah         /* BIOS: read sectors */
  movb  $8, %al            /* 8 sectors = 4 KB */
  movb  $0, %ch            /* cylinder 0 */
  movb  $2, %cl            /* start at sector 2 (1-based) */
  movb  $0, %dh            /* head 0 */
  movb  drive_no, %dl      /* drive number */
  int   $0x13
  jc    .Ldisk_err

  /* Pass drive number in DL and jump to stage2 */
  movb  drive_no, %dl
  jmp   $0x0000, $0xC000

.Ldisk_err:
  movb  $'E', %al
  call  print_char
.Lhalt:
  hlt
  jmp   .Lhalt

/* Print AL to BIOS teletype (VGA) and COM1.  Preserves all registers. */
print_char:
  pushw %ax
  pushw %bx
  pushw %dx
  movb  %al, %bl                /* stash char in BL */
  /* COM1: poll LSR bit 5 (THRE) until ready */
  movw  $0x03FD, %dx
1:
  inb   %dx, %al
  testb $0x20, %al
  jz    1b
  movw  $0x03F8, %dx
  movb  %bl, %al
  outb  %al, %dx
  /* BIOS teletype */
  movb  $0x0E, %ah
  movb  %bl, %al
  xorw  %bx, %bx
  int   $0x10
  popw  %dx
  popw  %bx
  popw  %ax
  ret

drive_no:
  .byte 0

  /* Pad to 510 bytes and add boot signature */
  .org  510
  .word 0xAA55
