/*
 * dos_trap.S --- INT 21h DOS ISR for i8086 real mode
 *
 * Uses the SAME stack layout as trap.S (INT 30h syscall):
 *   User stack (pushed by CPU + ISR prologue, low-to-high at user_SP):
 *     [+0 ] ES   [+2 ] DS  [+4 ] BP  [+6 ] DI  [+8 ] SI
 *     [+10] DX   [+12] CX  [+14] BX  [+16] AX
 *     [+18] IP   [+20] CS  [+22] FLAGS      (pushed by CPU on INT)
 *   Kernel stack (at SP after prologue):
 *     [+0 ..+33] 34-byte vfork-save slot  (reserved for uniformity)
 *     [+34]      user_SP
 *     [+36]      user_SS
 *
 * After dispatch we jump into trap.S's shared post-dispatch tail,
 * which decides whether the scratch frame should be copied back to
 * user memory before continuing with the generic ia16 switch/restore
 * path.  That tail handles: deferred context switch (zombie/blocked
 * via sys_exit), vfork parent-frame restore, sigreturn, signal
 * delivery, user SS:SP restore and the final IRET.
 *
 * The dispatcher reads/writes a `dos_regs_t` whose layout matches the
 * 24-byte GP+IRET frame at user_SP (see dos_bridge.h).  AX and FLAGS
 * modifications propagate back to the caller via the IRET pop.
 */

  .code16
  .text

  .global i16_dos_isr
  .global i16_dos_int20_isr
  .extern i16_dos_int21h_dispatch_native
  .extern i16_current_ksp
  .extern i16_trap_finish_scratch_frame

/* INT 20h (Program Terminate) — convert to INT 21h AH=4Ch, exit code 0.
 * Sets AX before falling through to i16_dos_isr so the dispatcher sees
 * function 4Ch. */
i16_dos_int20_isr:
  movw  $0x4C00, %ax
  /* fall through */

i16_dos_isr:
  /* CPU pushed FLAGS, CS, IP onto current SS:SP.  IF=0. */

  /* Save GP registers on the current stack in trap.S order */
  pushw %ax
  pushw %bx
  pushw %cx
  pushw %dx
  pushw %si
  pushw %di
  pushw %bp
  pushw %ds
  pushw %es

  /* Capture interrupted SS:SP */
  movw  %ss, %ax
  movw  %sp, %bp

  /* Switch to kernel stack if we came from user mode */
  testw %ax, %ax
  jz    .Lkstack_ready

  xorw  %bx, %bx
  movw  %bx, %ss
  movw  %ss:i16_current_ksp, %sp

.Lkstack_ready:
  /* Push interrupted SS:SP onto kernel stack */
  pushw %ax               /* user_SS */
  pushw %bp               /* user_SP  — points at GP frame on user stack */

  /* DS = ES = 0 for kernel data access */
  xorw  %ax, %ax
  movw  %ax, %ds
  movw  %ax, %es

  /* Reserve the 34-byte vfork-save slot above the C call chain so this
   * entry path produces the same kernel-stack layout as trap.S /
   * switch.S and can share their restore tail unchanged. */
  subw  $34, %sp

  /* Copy the 24-byte GP+IRET frame (ES..FLAGS) from user stack into
   * a kernel-stack scratch buffer.  The dispatcher works on the copy
   * so its regs->* stores do not require a user-segment override. */
  movw  %sp, %bp
  movw  36(%bp), %ax      /* user_SS (at kstack_top - 2) */
  movw  34(%bp), %si      /* user_SP (at kstack_top - 4) */

  subw  $24, %sp          /* scratch room for dos_regs_t copy */
  movw  %sp, %di
  movw  %ax, %ds          /* DS = user_SS (source) */
  movw  $12, %cx          /* 12 words = 24 bytes */
  cld
  rep movsw
  xorw  %ax, %ax
  movw  %ax, %ds          /* DS back to 0 for kernel */

  /* Call dispatcher: int i16_dos_int21h_dispatch_native(dos_regs_t *) */
  movw  %sp, %ax
  pushw %ax
  call  i16_dos_int21h_dispatch_native
  addw  $2, %sp

  /* Hand off to the shared ia16 post-dispatch tail.  It owns the
   * generic policy of whether this scratch frame should be copied back
   * to user memory or dropped because exec/exit/block changed the task
   * state, then continues through the common switch/restore path. */
  jmp   i16_trap_finish_scratch_frame
