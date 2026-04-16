/*
 * trap.S — INT 30h syscall handler for i8086 real mode
 *
 * User programs invoke syscalls via:
 *   AX = syscall number
 *   BX, CX, DX, SI, DI = arguments (a0-a4)
 *   INT 30h
 *   Return value in AX
 *
 * Same stack-switch pattern as switch.S: GP registers are saved on the
 * interrupted stack, then SS:SP is saved on the kernel stack.
 *
 * Kernel stack (4 bytes at pcb.sp during blocked syscalls):
 *   [SP+0] user_SS
 *   [SP+2] user_SP   (points to GP frame on interrupted stack)
 *
 * Interrupted stack (24 bytes):
 *   [uSP+0 ] ES
 *   [uSP+2 ] DS      (= user DS, used for user_ds argument)
 *   [uSP+4 ] BP
 *   [uSP+6 ] DI      (arg a4)
 *   [uSP+8 ] SI      (arg a3)
 *   [uSP+10] DX      (arg a2)
 *   [uSP+12] CX      (arg a1)
 *   [uSP+14] BX      (arg a0)
 *   [uSP+16] AX      (syscall number / return value)
 *   [uSP+18] IP      (pushed by INT)
 *   [uSP+20] CS      (pushed by INT)
 *   [uSP+22] FLAGS   (pushed by INT)
 */

  .code16
  .text

  .equ PCB_SVC_NEEDS_RESTART_OFFSET, 452

  .global i16_syscall_isr
  .global i16_trap_after_switch
  .global i16_trap_maybe_switch
  .extern i16_trap_should_skip_ret_store
  .extern i16_trap_should_switch_now

i16_syscall_isr:
  /* CPU pushed FLAGS, CS, IP onto current SS:SP.  IF=0. */

  /* Save all GP registers on the current stack */
  pushw %ax           /* syscall number */
  pushw %bx           /* a0 */
  pushw %cx           /* a1 */
  pushw %dx           /* a2 */
  pushw %si           /* a3 */
  pushw %di           /* a4 */
  pushw %bp
  pushw %ds           /* user DS — needed for user_ds arg */
  pushw %es

  /* Capture current SS:SP */
  movw  %ss, %ax
  movw  %sp, %bp

  /* Switch to kernel stack if not already there */
  testw %ax, %ax
  jz    .Lkstack_ready

  xorw  %bx, %bx
  movw  %bx, %ss
  movw  %ss:i16_current_ksp, %sp

.Lkstack_ready:
  /* Push interrupted SS:SP onto kernel stack */
  pushw %ax           /* user_SS */
  pushw %bp           /* user_SP (points to GP frame) */

  /* Set DS = ES = 0 for kernel data / string ops */
  xorw  %ax, %ax
  movw  %ax, %ds
  movw  %ax, %es

  /* Save the syscall frame pointer for sys_sigreturn.
   * This points at the [user_SP, user_SS] slot — the same value the
   * C side knows as i16_trap_frame_sp. */
  movw  %sp, %ss:i16_trap_frame_sp

  /* Reserve a 34-byte slot above the C call chain for sys_vfork to
   * stash the parent's GP+IRET frame (24B) plus the vfork stub's
   * callee-saved registers and return address (10B).  The child's
   * execve call inevitably pushes args onto the shared user stack,
   * overwriting those 10 bytes above the GP+IRET frame.
   *
   * The slot lives at [kernel_stack_top - 38, kernel_stack_top - 4).
   * All later C frames sit *below* this slot.
   *
   * On the restore path, we addw $34,%sp before the popw user_SP /
   * user_SS to skip past the slot.  i16_vfork_restore_frame copies the
   * slot's contents back to the user stack when current is a vfork
   * parent being resumed. */
  subw  $34, %sp

  /* Build args for i16_syscall_dispatch(nr, a0, a1, a2, a3, a4, user_ds).
   * GP regs are on the interrupted stack at user_SS:user_SP.
   * Read them via the saved pointers.  8086 can't use SP as base.
   * After the subw above, [user_SP, user_SS] are at offsets 34, 36. */
  movw  %sp, %bp
  movw  34(%bp), %bx    /* user_SP */
  movw  36(%bp), %cx    /* user_SS */

  /* Set DS = user_SS to read the GP frame */
  movw  %cx, %ds

  pushw 2(%bx)          /* user_ds = saved DS at uSP+2 */
  pushw 6(%bx)          /* a4 = DI at uSP+6 */
  pushw 8(%bx)          /* a3 = SI at uSP+8 */
  pushw 10(%bx)         /* a2 = DX at uSP+10 */
  pushw 12(%bx)         /* a1 = CX at uSP+12 */
  pushw 14(%bx)         /* a0 = BX at uSP+14 */
  pushw 16(%bx)         /* nr = AX at uSP+16 */

  /* Restore DS = 0 for kernel */
  xorw  %ax, %ax
  movw  %ax, %ds

  call  i16_syscall_dispatch

  addw  $14, %sp        /* clean 7 args */

  /* Keep the return value aside */
  movw  %ax, %dx

  /* Store return value into the saved AX slot on the interrupted stack.
   * Read user_SS:user_SP from kernel stack.  After the addw $14 above
   * the SP is back at the post-subw position, so [user_SP, user_SS]
   * are still at offsets 34, 36 relative to BP. */
  movw  %sp, %bp
  movw  34(%bp), %bx    /* user_SP */
  movw  36(%bp), %cx    /* user_SS */

  /* execve replaced the process image: build kernel stack frame
   * from PCB's exec_user_ss/sp and switch to it. */
  movw  %ss:exec_pending, %ax
  testw %ax, %ax
  jz    .Lcheck_restart
  movw  $0, %ss:exec_pending
  movw  %ss:current_core, %bx
  /* Build [user_SP, user_SS] at kernel_stack_top - 4.
   * PCB layout: sp(4B), kernel_stack_top(2B), exec_user_ss(2B),
   *             exec_user_sp(2B). Offsets: 4, 6, 8. */
  movw  4(%bx), %sp           /* SP = kernel_stack_top */
  pushw 6(%bx)                /* push exec_user_ss */
  pushw 8(%bx)                /* push exec_user_sp */
  subw  $34, %sp              /* reserve vfork-save slot (see .Lkstack_ready) */
  jmp   .Lmaybe_switch

.Lcheck_restart:
  /* Blocking syscalls rewind IP so INT 30h re-executes when resumed */
  pushw %bx
  movw  %ss:current_core, %bx
  xorw  %ax, %ax
  movb  %ss:PCB_SVC_NEEDS_RESTART_OFFSET(%bx), %al
  testb %al, %al
  jz    .Lno_restart
  movb  $0, %ss:PCB_SVC_NEEDS_RESTART_OFFSET(%bx)
  movw  $0, %ss:svc_restart
  popw  %bx
  /* Rewind IP on the interrupted stack: IP is at user_SP+18 */
  movw  %cx, %ds        /* DS = user_SS */
  subw  $2, 18(%bx)     /* IP -= 2 (back to INT 30h) */
  xorw  %ax, %ax
  movw  %ax, %ds        /* DS = 0 */
  jmp   .Lmaybe_switch

.Lno_restart:
  popw  %bx

.Lstore_ret:
  /* Only skip the AX write for the vfork parent case where the child is
   * still running on the parent's shared user stack. A normal deferred
   * switch must still commit the current syscall's return value before
   * we hand the CPU to another runnable task. */
  pushw %bx
  pushw %cx
  pushw %dx
  call  i16_trap_should_skip_ret_store
  popw  %dx
  popw  %cx
  popw  %bx
  testw %ax, %ax
  jnz   .Lmaybe_switch

  /* Normal return: store syscall result in saved AX slot */
  movw  %cx, %ds        /* DS = user_SS */
  movw  %dx, 16(%bx)    /* saved AX = return value */
  xorw  %ax, %ax
  movw  %ax, %ds        /* DS = 0 */

.Lmaybe_switch:
i16_trap_maybe_switch:
  /* Check if a context switch was requested (e.g. by sys_waitpid) */
  movw  %ss:switch_pending, %ax
  testw %ax, %ax
  jz    .Lafter_switch
  call  i16_trap_should_switch_now
  testw %ax, %ax
  jz    .Lafter_switch
  movw  $0, %ss:switch_pending

  movw  %ss:current_core, %bx
  movw  %sp, (%bx)

  call  sched_next
  movw  %ax, %ss:current_core
  movw  %ax, %bx
  movw  (%bx), %sp

  /* Update i16_current_ksp from new PCB (offset 4) */
  movw  4(%bx), %ax
  movw  %ax, %ss:i16_current_ksp

.Lafter_switch:
i16_trap_after_switch:
  /* Restore vfork parent's saved GP+IRET frame to user stack.
   * If current is a vfork parent whose frame was saved on the kernel
   * stack, this copies it back before the normal restore path. */
  call  i16_vfork_restore_frame

  /* sys_sigreturn may ask us to resume from an older saved frame */
  movw  %ss:i16_sigreturn_restore_sp, %ax
  testw %ax, %ax
  jz    .Lcheck_signal
  movw  $0, %ss:i16_sigreturn_restore_sp
  movw  %ax, %sp
  jmp   .Lrestore

.Lcheck_signal:
  /* User-signal delivery */
  /* 8086 PUSH SP pushes the decremented value, unlike later x86.
   * Pass the pre-push stack pointer explicitly so signal_check() sees
   * the real frame base on 8086/80186 as well as 286+. */
  movw  %sp, %ax
  pushw %ax
  call  signal_check
  addw  $2, %sp
  movw  %ax, %sp

.Lrestore:
  /* Skip the 34-byte vfork-save slot we reserved at trap entry. */
  addw  $34, %sp

  /* Restore interrupted SS:SP */
  popw  %bp             /* user_SP */
  popw  %ax             /* user_SS */

  movw  %ax, %ss
  movw  %bp, %sp

  /* Pop GP registers from interrupted stack */
  popw  %es
  popw  %ds
  popw  %bp
  popw  %di
  popw  %si
  popw  %dx
  popw  %cx
  popw  %bx
  popw  %ax             /* return value */

  iret
