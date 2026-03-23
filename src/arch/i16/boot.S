/*
 * boot.S -- i8086 real-mode boot entry point
 *
 * For P-1 (QEMU testing), this is loaded via a minimal boot sector or
 * QEMU -kernel.  The entry point sets up segment registers and stack,
 * zeroes BSS, and jumps to kmain().
 *
 * Memory layout (small model, CS = DS = SS):
 *   0x0000 - .text (code)
 *   .text end - .data (initialized data)
 *   .data end - .bss (uninitialized, zeroed here)
 *   .bss end  - stack top (grows downward)
 *
 * Assembled with ia16-elf-gcc (GAS syntax, AT&T).
 */

  .code16
  .section .text.entry, "ax"
  .global _start

_start:
  cli

  /* Set up segments: DS = ES = SS = CS (small model) */
  movw  %cs, %ax
  movw  %ax, %ds
  movw  %ax, %es
  movw  %ax, %ss

  /* Stack at top of segment (just below 64 KB boundary) */
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
