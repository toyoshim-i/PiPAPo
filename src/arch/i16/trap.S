/*
 * trap.S — INT 30h syscall handler for i8086 real mode
 *
 * User programs invoke syscalls via:
 *   AX = syscall number
 *   BX, CX, DX, SI, DI = arguments (a0-a4)
 *   INT 30h
 *   Return value in AX
 *
 * The handler saves user registers, sets DS=0 for kernel data access,
 * calls syscall_dispatch(), restores registers, and returns via IRET.
 *
 * Stack frame layout (same as switch.S timer ISR):
 *   [SP+0 ] ES
 *   [SP+2 ] DS
 *   [SP+4 ] BP
 *   [SP+6 ] DI     (arg a4)
 *   [SP+8 ] SI     (arg a3)
 *   [SP+10] DX     (arg a2)
 *   [SP+12] CX     (arg a1)
 *   [SP+14] BX     (arg a0)
 *   [SP+16] AX     (syscall number / return value)
 *   [SP+18] IP     (pushed by INT)
 *   [SP+20] CS     (pushed by INT)
 *   [SP+22] FLAGS  (pushed by INT)
 */

  .code16
  .text

  .global i16_syscall_isr

i16_syscall_isr:
  /* CPU pushed FLAGS, CS, IP */

  /* Save all registers */
  pushw %ax           /* syscall number */
  pushw %bx           /* a0 */
  pushw %cx           /* a1 */
  pushw %dx           /* a2 */
  pushw %si           /* a3 */
  pushw %di           /* a4 */
  pushw %bp
  pushw %ds
  pushw %es

  /* Set DS = ES = 0 for kernel data / string ops */
  xorw  %ax, %ax
  movw  %ax, %ds
  movw  %ax, %es

  /* Save the actual syscall frame pointer for sys_sigreturn. */
  movw  %sp, i16_trap_frame_sp

  /* Build args for i16_syscall_dispatch(nr, a0, a1, a2, a3, a4, user_ds)
   * Read from the saved frame on the stack. */
  movw  %sp, %bp

  pushw 2(%bp)        /* user_ds = saved DS */
  pushw 6(%bp)        /* a4 = DI */
  pushw 8(%bp)        /* a3 = SI */
  pushw 10(%bp)       /* a2 = DX */
  pushw 12(%bp)       /* a1 = CX */
  pushw 14(%bp)       /* a0 = BX */
  pushw 16(%bp)       /* nr = AX (syscall number) */

  call  i16_syscall_dispatch

  addw  $14, %sp      /* clean 7 args */

  /* Keep the return value aside while we inspect trap-return state. */
  movw  %ax, %dx
  movw  %sp, %bp

  /* execve replaced the process image: switch to current->sp before
   * touching the saved AX/IP slots. */
  movw  exec_pending, %ax
  testw %ax, %ax
  jz    .Lcheck_restart
  movw  $0, exec_pending
  movw  current_core, %bx
  movw  (%bx), %sp
  jmp   .Lmaybe_switch

.Lcheck_restart:
  /* Blocking syscalls keep the original AX/BX..DI frame and rewind IP so
   * INT 30h re-executes when the process runs again. */
  movw  svc_restart, %ax
  testw %ax, %ax
  jz    .Lstore_ret
  movw  $0, svc_restart
  subw  $2, 18(%bp)   /* back to INT 30h */
  jmp   .Lmaybe_switch

.Lstore_ret:
  /* Normal return: store the syscall result in the saved AX slot. */
  movw  %dx, 16(%bp)

.Lmaybe_switch:
  /* sys_waitpid/sys_execve/trace stops can pend a switch directly from the
   * syscall path.  Do the same save/restore sequence as the timer ISR so we
   * do not return to a blocked task until the next PIT tick. */
  movw  i16_switch_pending, %ax
  testw %ax, %ax
  jz    .Lafter_switch
  movw  $0, i16_switch_pending

  movw  current_core, %bx
  movw  %sp, (%bx)

  call  sched_next
  movw  %ax, current_core
  movw  %ax, %bx
  movw  (%bx), %sp

.Lafter_switch:

  /* sys_sigreturn may ask us to resume from an older saved frame. */
  movw  i16_sigreturn_restore_sp, %ax
  testw %ax, %ax
  jz    .Lcheck_signal
  movw  $0, i16_sigreturn_restore_sp
  movw  %ax, %sp
  jmp   .Lrestore

.Lcheck_signal:
  /* User-signal delivery can replace the frame we restore from. */
  pushw %sp
  call  signal_check
  addw  $2, %sp
  movw  %ax, %sp

  /* Restore all registers (AX will have the return value) */
.Lrestore:
  popw  %es
  popw  %ds
  popw  %bp
  popw  %di
  popw  %si
  popw  %dx
  popw  %cx
  popw  %bx
  popw  %ax           /* return value */

  iret
