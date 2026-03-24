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
  movw  $0x7C00, %sp      /* reuse stage1 stack area */
  sti

  /* DL = boot drive number (passed by stage1) */
  movb  %dl, boot_drive

  /* Print '2' to show stage2 is running */
  movb  $0x0E, %ah
  movb  $'2', %al
  xorw  %bx, %bx
  int   $0x10

  call  stage2_main

  /* Should not return */
.Lhalt:
  hlt
  jmp   .Lhalt

  .global boot_drive
boot_drive:
  .byte 0
