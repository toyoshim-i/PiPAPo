/*
 * boot.S -- i8086 real-mode boot entry point
 *
 * BIOS loads this boot sector to 0x0000:0x7C00 and jumps here.
 * Sets up segment registers, stack, zeroes BSS, calls kmain().
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

  /* Stack grows down from 0x7C00 (below our code) */
  movw  $__stack_top, %sp

  /* Zero BSS */
  movw  $__bss_start, %di
  movw  $__bss_end, %cx
  subw  %di, %cx
  shrw  $1, %cx        /* count in words */
  xorw  %ax, %ax
  cld
  rep   stosw

  sti

  /* Call C entry point */
  call  kmain

  /* If kmain returns, halt */
.Lhalt:
  hlt
  jmp   .Lhalt
