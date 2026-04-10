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

  /* Snapshot the entry-time register state into CS-resident scratch
   * slots BEFORE we touch any segment register or stack pointer.  On a
   * warm-reboot path the previous wild jump may have left segregs in
   * garbage state — this is the only place we can capture it.  Slots
   * live in .text (CS-accessible without DS) and are read out into
   * panic_frame by copy_entry_snapshot() once DS=0 is set up. */
  movw  %ax, %cs:entry_ax
  movw  %bx, %cs:entry_bx
  movw  %cx, %cs:entry_cx
  movw  %dx, %cs:entry_dx
  movw  %si, %cs:entry_si
  movw  %di, %cs:entry_di
  movw  %bp, %cs:entry_bp
  movw  %sp, %cs:entry_sp
  movw  %ds, %ax
  movw  %ax, %cs:entry_ds
  movw  %es, %ax
  movw  %ax, %cs:entry_es
  movw  %ss, %ax
  movw  %ax, %cs:entry_ss

  /* Set DS = ES = SS = 0 (flat real-mode addressing) */
  xorw  %ax, %ax
  movw  %ax, %ds
  movw  %ax, %es
  movw  %ax, %ss

  /* Boot stack = top of kernel stack region (slot 7 top - 4B guard).
   * 0xE380 + 128 (slot 0) + 7*1024 (slots 1-7) = 0x10000; SP=0xFFFC.
   * After sched_start(), each process uses its own assigned stack. */
  movw  $0xFFFC, %sp

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

  /* Copy entry-time CS scratch snapshot into panic_frame (in DS=0). */
  call  copy_entry_snapshot

  sti

  call  kmain

.Lhalt:
  hlt
  jmp   .Lhalt
