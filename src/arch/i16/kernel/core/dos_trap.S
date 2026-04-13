/*
 * dos_trap.S --- INT 21h DOS ISR for i8086 real mode
 *
 * Saves the interrupted registers in dos_regs_t layout on the user
 * stack, copies them to the kernel stack for the C dispatcher, and
 * copies modified values back after return.
 *
 * dos_regs_t layout (20 bytes, low-to-high on stack):
 *   [+0]  AX   [+2]  BX   [+4]  CX   [+6]  DX
 *   [+8]  SI   [+10] DI   [+12] BP   [+14] DS
 *   [+16] ES   [+18] FLAGS
 */

  .code16
  .text

  .global i16_dos_isr
  .extern i16_dos_int21h_dispatch_native
  .extern i16_current_ksp

i16_dos_isr:
  /* CPU pushed FLAGS, CS, IP onto current SS:SP.  IF=0. */

  /* Capture current SP to access FLAGS in the IRET frame */
  movw  %sp, %bp

  /* Save registers on the interrupted stack in dos_regs_t order
   * (pushed in reverse so the struct reads naturally from low addr). */
  pushw 4(%bp)        /* flags (copy from IRET frame) */
  pushw %es
  pushw %ds
  pushw %bp
  pushw %di
  pushw %si
  pushw %dx
  pushw %cx
  pushw %bx
  pushw %ax

  /* Capture user SS:SP.  SP now points at dos_regs_t on user stack. */
  movw  %ss, %ax          /* ax = user_SS */
  movw  %sp, %bp          /* bp = user_SP */

  /* Switch to kernel stack if not already there */
  testw %ax, %ax
  jz    .Lkstack_ready

  xorw  %bx, %bx
  movw  %bx, %ss
  movw  %ss:i16_current_ksp, %sp

.Lkstack_ready:
  /* Save user_SS:user_SP on kernel stack */
  pushw %ax               /* user_SS */
  pushw %bp               /* user_SP */

  /* ── Copy dos_regs_t (20 bytes) from user stack to kernel stack ── */
  subw  $20, %sp          /* make room on kernel stack */

  movw  %ax, %ds          /* DS = user_SS (source segment) */
  xorw  %ax, %ax
  movw  %ax, %es          /* ES = 0 (kernel, destination segment) */

  movw  %bp, %si          /* SI = user_SP (source offset) */
  movw  %sp, %di          /* DI = kernel stack (dest offset) */
  movw  $10, %cx          /* 10 words = 20 bytes */
  cld
  rep movsw

  /* Restore DS = 0 for kernel */
  xorw  %ax, %ax
  movw  %ax, %ds

  /* ── Call C dispatcher ─────────────────────────────────────────── */
  /* i16_dos_int21h_dispatch_native(dos_regs_t *regs)
   * regs points to the kernel-stack copy at SP */
  movw  %sp, %ax
  pushw %ax
  call  i16_dos_int21h_dispatch_native
  addw  $2, %sp

  /* ── Copy modified dos_regs_t back to user stack ───────────────── */
  movw  %sp, %bp
  movw  22(%bp), %ax      /* user_SS (at SP+20+2) */
  movw  20(%bp), %di      /* user_SP = dest offset */

  movw  %ax, %es          /* ES = user_SS (destination segment) */
  movw  %sp, %si          /* SI = kernel copy (source offset, DS=0) */
  movw  $10, %cx
  cld
  rep movsw

  /* Clean up kernel stack: drop the 20-byte copy */
  addw  $20, %sp

  /* ── Restore user stack and return ─────────────────────────────── */
  popw  %bp               /* user_SP */
  popw  %ax               /* user_SS */

  movw  %ax, %ss
  movw  %bp, %sp

  /* Pop modified registers from user stack (dos_regs_t order) */
  popw  %ax
  popw  %bx
  popw  %cx
  popw  %dx
  popw  %si
  popw  %di
  popw  %bp
  popw  %ds
  popw  %es
  popw  %bx               /* modified FLAGS */

  /* Update FLAGS in the IRET frame (at SP+4).
   * 8086 has no SP-relative addressing; use BP. */
  movw  %sp, %bp
  movw  %bx, 4(%bp)

  iret
