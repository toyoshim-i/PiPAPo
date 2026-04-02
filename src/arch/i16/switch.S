/*
 * switch.S -- Timer ISR and context switch for i8086 real mode
 *
 * PIT IRQ 0 fires INT 08h at 100 Hz.  The ISR saves GP registers on
 * the current stack (user or kernel), switches to the per-process
 * kernel stack (SS=0), and saves the original SS:SP there.  The
 * hardware-pushed FLAGS/CS/IP and the GP register frame stay on the
 * interrupted stack.  On exit, user SS:SP is restored and IRET pops
 * FLAGS/CS/IP from there.
 *
 * Kernel stack (4 bytes at pcb.sp):
 *   [SP+0] user_SS
 *   [SP+2] user_SP   (points to saved GP regs on interrupted stack)
 *
 * Interrupted stack (24 bytes, GP + hardware frame):
 *   [uSP+0 ] ES
 *   [uSP+2 ] DS
 *   [uSP+4 ] BP
 *   [uSP+6 ] DI
 *   [uSP+8 ] SI
 *   [uSP+10] DX
 *   [uSP+12] CX
 *   [uSP+14] BX
 *   [uSP+16] AX
 *   [uSP+18] IP     (pushed by INT)
 *   [uSP+20] CS     (pushed by INT)
 *   [uSP+22] FLAGS  (pushed by INT)
 *
 * When interrupted from kernel (SS=0, e.g. PID 0 idle loop), the
 * GP regs and user_SS:SP are all on the same kernel stack.
 * The restore path is uniform: always mov user_SS→SS, user_SP→SP,
 * then pop GP regs + IRET.
 */

  .code16
  .text

  .global i16_timer_isr

i16_timer_isr:
  /* CPU has pushed FLAGS, CS, IP (6 bytes) onto current SS:SP.
   * IF=0 (disabled by INT hardware). */

  /* Save all GP registers on the CURRENT stack */
  pushw %ax
  pushw %bx
  pushw %cx
  pushw %dx
  pushw %si
  pushw %di
  pushw %bp
  pushw %ds
  pushw %es
  /* Current stack now has 24 bytes: ES..AX + IP/CS/FLAGS */

  /* Capture current SS:SP */
  movw  %ss, %ax        /* AX = interrupted SS */
  movw  %sp, %bp        /* BP = interrupted SP (top of 24-byte frame) */

  /* If already on kernel stack (SS=0), skip the switch */
  testw %ax, %ax
  jz    .Lkstack_ready

  /* Switch to kernel stack.
   * 8086 guarantees: instruction after SS load runs with interrupts
   * inhibited, so the SS:SP pair is set atomically. */
  xorw  %bx, %bx
  movw  %bx, %ss
  movw  i16_current_ksp, %sp

.Lkstack_ready:
  /* Push interrupted SS:SP onto kernel stack */
  pushw %ax             /* user_SS */
  pushw %bp             /* user_SP (points to GP frame) */

  /* Set DS = ES = 0 for kernel data access */
  xorw  %ax, %ax
  movw  %ax, %ds
  movw  %ax, %es

  /* Call kernel timer tick handler */
  xorw  %ax, %ax        /* from_user = 0 (TODO: detect from saved SS) */
  pushw %ax
  call  sched_timer_tick
  addw  $2, %sp

  /* Send EOI to 8259A PIC (non-specific) */
  movb  $0x20, %al
  outb  %al, $0x20

  /* Check context switch flag */
  movw  i16_switch_pending, %ax
  testw %ax, %ax
  jz    .Lno_switch

  /* Clear the flag */
  movw  $0, i16_switch_pending

  /* Save current kernel SP into current PCB (offset 0) */
  movw  current_core, %bx
  movw  %sp, (%bx)

  /* Pick next process */
  call  sched_next

  /* Update current_core[0] */
  movw  %ax, current_core

  /* Load new process's kernel SP */
  movw  %ax, %bx
  movw  (%bx), %sp

  /* Update i16_current_ksp from new PCB.
   * kernel_stack_top is at PCB offset 4 (sp is uint32_t = 4 bytes). */
  movw  4(%bx), %ax
  movw  %ax, i16_current_ksp

.Lno_switch:
  /* Restore interrupted SS:SP */
  popw  %bp             /* user_SP */
  popw  %ax             /* user_SS */

  /* Switch back to interrupted stack.
   * For kernel (SS=0): already SS=0, just sets SP back.
   * For user (SS=proc_seg): switches to user segment. */
  movw  %ax, %ss
  movw  %bp, %sp

  /* Pop GP registers from the interrupted stack */
  popw  %es
  popw  %ds
  popw  %bp
  popw  %di
  popw  %si
  popw  %dx
  popw  %cx
  popw  %bx
  popw  %ax

  iret
