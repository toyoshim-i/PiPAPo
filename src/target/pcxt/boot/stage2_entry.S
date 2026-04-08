/*
 * stage2_entry.S — Stage2 entry stub for IBM PC
 *
 * Loaded at 0x0000:0x0800 by stage1.  Sets up segments, saves
 * the boot drive number, and calls stage2_main() in C.
 */

  .code16
  .section .text, "ax"
  .global _start2

_start2:
  cli
  xorw  %ax, %ax
  movw  %ax, %ds
  movw  %ax, %es
  movw  %ax, %ss
  movw  $0xC000, %sp      /* stack below stage2 (0xC000), grows down */
  sti

  /* DL = boot drive number (passed by stage1) */
  movb  %dl, boot_drive

  /* Print "PA" — stage2 portion of the progressive banner.
   * print_char writes to both BIOS teletype (VGA) and COM1. */
  movb  $'P', %al
  call  print_char
  movb  $'A', %al
  call  print_char

  call  stage2_main

  /* Should not return */
.Lhalt:
  hlt
  jmp   .Lhalt

/* Print AL to BIOS teletype (VGA) and COM1.  Preserves all registers.
 * Exposed as global so stage2.c can call it via puts_bios. */
  .global print_char
print_char:
  pushw %ax
  pushw %bx
  pushw %dx
  movb  %al, %bl
  movw  $0x03FD, %dx
1:
  inb   %dx, %al
  testb $0x20, %al
  jz    1b
  movw  $0x03F8, %dx
  movb  %bl, %al
  outb  %al, %dx
  movb  $0x0E, %ah
  movb  %bl, %al
  xorw  %bx, %bx
  int   $0x10
  popw  %dx
  popw  %bx
  popw  %ax
  ret

  .global boot_drive
boot_drive:
  .byte 0
