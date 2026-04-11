/*
 * stage1_common.inc — Shared boot code for floppy and HDD stage1
 *
 * Included by stage1.S (floppy, with BPB) and stage1_hdd.S (MBR).
 * Entry: CPU at .Lstart, DL = boot drive number from BIOS.
 *
 * Sets up segments, saves boot drive, prints "Pi" banner, loads
 * 8 sectors of stage2 to 0x0000:0xC000, and far-jumps there.
 */

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

  /* Load 8 sectors (stage2) from sectors 1-8 to 0x0000:0xC000 */
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
  pushw %cx
  pushw %dx
  movb  %al, %bl                /* stash char in BL */
  /* COM1: poll LSR bit 5 (THRE), but don't let a missing UART hang boot. */
  movw  $0x03FD, %dx
  movw  $0xFFFF, %cx
1:
  inb   %dx, %al
  testb $0x20, %al
  jnz   2f
  loop  1b
  jmp   3f
2:
  movw  $0x03F8, %dx
  movb  %bl, %al
  outb  %al, %dx
3:
  /* BIOS teletype */
  movb  $0x0E, %ah
  movb  %bl, %al
  xorw  %bx, %bx
  int   $0x10
  popw  %dx
  popw  %cx
  popw  %bx
  popw  %ax
  ret

drive_no:
  .byte 0
