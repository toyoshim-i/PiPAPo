/*
 * boot.S -- i8086 real-mode boot sector (sector 0)
 *
 * BIOS loads this 512-byte sector to 0x0000:0x7C00 and jumps here.
 * We load the remaining kernel sectors from floppy into 0x7E00+
 * using BIOS INT 13h, zero BSS, set up the stack, and call kmain().
 *
 * Assembled with ia16-elf-gcc (GAS syntax, AT&T).
 */

  .code16
  .section .text.entry, "ax"
  .global _start

_start:
  cli

  /* Set up segments: DS = ES = SS = 0 (flat real-mode addressing) */
  xorw  %ax, %ax
  movw  %ax, %ds
  movw  %ax, %es
  movw  %ax, %ss

  /* Stack grows down from 0x7C00 (below boot sector) */
  movw  $0x7C00, %sp

  /* Save boot drive number (BIOS passes it in DL) */
  movb  %dl, boot_drive

  /* Load remaining kernel sectors from floppy into 0x7E00 */
  movw  $0x7E00, %bx      /* ES:BX = destination (ES=0) */
  movb  $0x02, %ah        /* BIOS: read sectors */
  movb  $__kernel_sectors, %al  /* number of sectors */
  movb  $0x00, %ch        /* cylinder 0 */
  movb  $0x02, %cl        /* start at sector 2 (1-based; sector 1 = boot) */
  movb  $0x00, %dh        /* head 0 */
  movb  boot_drive, %dl   /* drive number */
  int   $0x13
  jc    .Ldisk_err

  /* Zero BSS */
  movw  $__bss_start, %di
  movw  $__bss_end, %cx
  subw  %di, %cx
  jz    .Lbss_done         /* skip if BSS is empty */
  shrw  $1, %cx            /* count in words */
  xorw  %ax, %ax
  cld
  rep   stosw
.Lbss_done:

  /* Set stack to top of allocated stack area */
  movw  $__stack_top, %sp

  sti

  /* Call C entry point */
  call  kmain

  /* If kmain returns, halt */
.Lhalt:
  hlt
  jmp   .Lhalt

.Ldisk_err:
  /* Print 'E' on screen and halt */
  movb  $0x0E, %ah
  movb  $'E', %al
  xorw  %bx, %bx
  int   $0x10
  jmp   .Lhalt

  /* Boot drive number (saved by _start) */
boot_drive:
  .byte 0
