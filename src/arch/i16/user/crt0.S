/*
 * crt0.S — Minimal C runtime for PPAP i16 user-space programs
 *
 * Entry point: _start
 *   1. Zero .bss
 *   2. Call main(0, NULL)
 *   3. Call _exit(main's return value)
 *
 * The current i16 ELF loader does not yet build argc/argv on the user
 * stack, so crt0 always enters main with zero arguments.
 */

  .code16
  .section .text.crt0, "ax"
  .global _start
  .extern main
  .extern _exit

_start:
  cld

  /* Zero .bss with ES=DS=process segment (set by elf16_loader). */
  xorw  %ax, %ax
  movw  $__bss_start, %di
  movw  $__bss_end, %cx
  subw  %di, %cx
  jbe   .Lbss_done
  rep
  stosb

.Lbss_done:
  pushw $0              /* argv = NULL */
  pushw $0              /* argc = 0 */
  call  main
  addw  $4, %sp

  pushw %ax
  call  _exit

.Lhang:
  hlt
  jmp   .Lhang
