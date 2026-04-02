/*
 * boot.S -- i8086 kernel entry point
 *
 * Stage2 loads the kernel flat binary to 0x2000 and jumps here.
 * All segments = 0 (flat real-mode addressing with near pointers).
 */

  .code16
  .section .text.entry, "ax"
  .global _start

_start:
  cli

  /* Set DS = ES = SS = 0 (flat real-mode addressing) */
  xorw  %ax, %ax
  movw  %ax, %ds
  movw  %ax, %es
  movw  %ax, %ss

  /* Boot stack = top of kernel_stack[0] (0xF000 + 1 KB) */
  movw  $0xF400, %sp

  /* Zero BSS */
  movw  $__bss_start, %di
  movw  $__bss_end, %cx
  subw  %di, %cx
  jz    .Lbss_done
  shrw  $1, %cx
  xorw  %ax, %ax
  cld
  rep   stosw
.Lbss_done:

  sti

  call  kmain

.Lhalt:
  hlt
  jmp   .Lhalt
