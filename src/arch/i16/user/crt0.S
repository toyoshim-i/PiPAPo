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
  /* Stack on entry: [sp+0] = argc, [sp+2..] = argv[0..N], NULL,
   *                 envp[0..M], NULL, strings.
   * Read argc/argv without modifying the area above SP, and leave the
   * stack pointer untouched so _uclib_init_env can also see argc/argv. */
  movw  %sp, %bp
  movw  0(%bp), %ax     /* ax = argc */
  leaw  2(%bp), %bx     /* bx = &argv[0] */

  /* _uclib_init_env(argc, argv) — sets the `environ` global. */
  pushw %bx
  pushw %ax
  call  _uclib_init_env
  addw  $4, %sp

  /* Reload argc/argv from the initial stack and call main. */
  movw  0(%bp), %ax
  leaw  2(%bp), %bx
  pushw %bx             /* argv (grows below argc, safe) */
  pushw %ax             /* argc */
  call  main
  addw  $4, %sp

  pushw %ax
  call  _exit

.Lhang:
  hlt
  jmp   .Lhang
