/*
 * switch.S -- Timer ISR and context switch for i8086 real mode
 *
 * PIT IRQ 0 fires INT 08h at 100 Hz.  The ISR saves all registers,
 * calls the C tick handler, sends EOI to the 8259A PIC, checks the
 * i16_switch_pending flag, and performs a context switch if set.
 *
 * Register save order (9 pushw = 18 bytes software + 6 bytes hardware):
 *   [SP+0 ] ES
 *   [SP+2 ] DS
 *   [SP+4 ] BP
 *   [SP+6 ] DI
 *   [SP+8 ] SI
 *   [SP+10] DX
 *   [SP+12] CX
 *   [SP+14] BX
 *   [SP+16] AX
 *   [SP+18] IP   (pushed by INT)
 *   [SP+20] CS   (pushed by INT)
 *   [SP+22] FLAGS (pushed by INT)
 *
 * Total frame: 24 bytes.  pcb.sp points to SP+0 (ES slot).
 *
 * Note: 8086 has no PUSHA/POPA (those are 80186+).
 */

  .code16
  .text

  .global i16_timer_isr

i16_timer_isr:
  /* CPU has pushed FLAGS, CS, IP (6 bytes) onto the current stack */

  /* Save all general-purpose registers + segment registers */
  pushw %ax
  pushw %bx
  pushw %cx
  pushw %dx
  pushw %si
  pushw %di
  pushw %bp
  pushw %ds
  pushw %es

  /* Set DS = ES = 0 for kernel data access (flat model) */
  xorw  %ax, %ax
  movw  %ax, %ds
  movw  %ax, %es

  /* Call kernel timer tick handler */
  xorw  %ax, %ax          /* from_user = 0 (no user/kernel split in real mode) */
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

  /* Save current SP into current PCB.
   * current_core[0] is a pcb_t* — SP is at offset 0 (PCB_SP_OFFSET). */
  movw  current_core, %bx  /* BX = current_core[0] = pcb_t* */
  movw  %sp, (%bx)         /* pcb->sp = SP */

  /* Pick next process: sched_next() returns pcb_t* in AX */
  call  sched_next

  /* Update current_core[0] */
  movw  %ax, current_core

  /* Restore SP from new PCB */
  movw  %ax, %bx
  movw  (%bx), %sp         /* SP = new pcb->sp */

.Lno_switch:
  /* Restore all registers */
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
