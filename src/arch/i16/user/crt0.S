/*
 * crt0.S — Minimal C runtime for PPAP i16 user-space programs
 *
 * Entry point: _start
 *   1. Zero .bss
 *   2. Read argc/argv from user stack (placed by elf16_loader)
 *   3. Call main(argc, argv)
 *   4. Call _exit(main's return value)
 *
 * Stack on entry (placed by elf16_loader, above the ISR frame):
 *   [sp+0]  argc
 *   [sp+2]  argv[0], argv[1], ..., NULL
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
  /* Stack on entry: [sp+0] = argc, [sp+2..] = argv[0..N], NULL, strings.
   * Move SP past the argv data so main's stack frames cannot overwrite
   * the live argv[] array.  Compute argc and argv pointer first, then
   * reset SP to just below the argc slot. */
  /* Stack: [sp+0] = argc, [sp+2] = argv[0], argv[1], ..., NULL
   * Read without modifying the argv data above SP. */
  movw  %sp, %bp
  movw  0(%bp), %ax     /* ax = argc */
  leaw  2(%bp), %bx     /* bx = &argv[0] */
  pushw %bx             /* argv (grows below argc, safe) */
  pushw %ax             /* argc */
  call  main
  addw  $4, %sp

  pushw %ax
  call  _exit

.Lhang:
  hlt
  jmp   .Lhang
