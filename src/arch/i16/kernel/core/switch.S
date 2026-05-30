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
  .extern i16_vfork_restore_frame

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
   * inhibited, so the SS:SP pair is set atomically.
   * Use %ss: prefix because DS may be garbage (compiler scratch). */
  xorw  %bx, %bx
  movw  %bx, %ss
  movw  %ss:i16_current_ksp, %sp

.Lkstack_ready:
  /* Push interrupted SS:SP onto kernel stack */
  pushw %ax             /* user_SS */
  pushw %bp             /* user_SP (points to GP frame) */

  /* Reserve a 34-byte vfork-save slot above the C call chain.
   * Matches trap.S i16_syscall_isr — every entry that may end up as
   * a saved kernel SP for a suspended process uses the same layout,
   * so the shared restore path can always `addw $34,%sp` to skip
   * past the slot before popping user_SP/user_SS.  Wasted on the
   * common case where no vfork is in progress, but only 34 bytes. */
  subw  $34, %sp

  /* Set DS = ES = 0 for kernel data access */
  xorw  %ax, %ax
  movw  %ax, %ds
  movw  %ax, %es

  incw  %ss:i16_irq_depth

  /* Call kernel timer tick handler.
   * user_SS is 0 when the timer interrupted kernel code and non-zero
   * when it interrupted user mode.  After the 34-byte reserve above,
   * user_SS lives at 36(%bp). */
  movw  %sp, %bp
  movw  36(%bp), %ax
  testw %ax, %ax
  jz    .Ltick_from_kernel
  movw  $1, %ax
  jmp   .Ltick_arg_ready

.Ltick_from_kernel:
  xorw  %ax, %ax

.Ltick_arg_ready:
  pushw %ax
  call  sched_timer_tick
  addw  $2, %sp

  /* Chain to the original BIOS INT 08h handler.  It handles floppy
   * motor timeout and sends EOI — we must NOT send EOI ourselves. */
  call  i16_bios_timer_chain

  /* Preempt from any state.  The core↔VFS entry stubs are reentrant
   * across context switch via per-PCB shadow of saved_cs/ip / vfs_saved_cs/ip
   * (see the shadow-swap block below).  BIOS call sites that share state
   * with the timer's INT 08h chain (INT 13h, INT 16h) mask IRQ 0 for the
   * duration of the call, so the timer stays pending until they return. */
  movw  %ss:switch_pending, %ax
  testw %ax, %ax
  jz    .Lno_switch

  /* Clear the flag */
  movw  $0, %ss:switch_pending

  /* Save current kernel SP into current PCB (offset 0) */
  movw  %ss:current_core, %bx
  movw  %sp, (%bx)

  /* Save the core/VFS entry-stub save globals into the outgoing PCB.
   * The entry stubs (core_entries.S / vfs_entries.S) store the
   * caller's far-return CS:IP in these globals between stub entry
   * and the matching lret.  Shadowing them per-PCB on every switch
   * makes the stubs reentrant across cooperative yield and timer
   * preemption — stubs themselves are unchanged. */
  movw  %ss:saved_cs,     %ax
  movw  %ax, 10(%bx)           /* PCB.core_stub_saved_cs (offset 10) */
  movw  %ss:saved_ip,     %ax
  movw  %ax, 12(%bx)           /* PCB.core_stub_saved_ip (offset 12) */
  movw  %ss:vfs_saved_cs, %ax
  movw  %ax, 14(%bx)           /* PCB.vfs_stub_saved_cs  (offset 14) */
  movw  %ss:vfs_saved_ip, %ax
  movw  %ax, 16(%bx)           /* PCB.vfs_stub_saved_ip  (offset 16) */

  /* Pick next process */
  call  sched_next

  /* Update current_core[0] */
  movw  %ax, %ss:current_core

  /* Load new process's kernel SP */
  movw  %ax, %bx
  movw  (%bx), %sp

  /* Restore the new process's shadow copies of the stub-save globals. */
  movw  10(%bx), %ax
  movw  %ax, %ss:saved_cs
  movw  12(%bx), %ax
  movw  %ax, %ss:saved_ip
  movw  14(%bx), %ax
  movw  %ax, %ss:vfs_saved_cs
  movw  16(%bx), %ax
  movw  %ax, %ss:vfs_saved_ip

  /* Update i16_current_ksp from new PCB.
   * kernel_sp is at PCB offset 4 (sp is uint32_t = 4 bytes). */
  movw  4(%bx), %ax
  movw  %ax, %ss:i16_current_ksp

.Lno_switch:
  decw  %ss:i16_irq_depth

  /* Restore vfork parent's saved GP+IRET frame to user stack.
   * The parent may resume via timer ISR (without passing through trap.S),
   * so restore must also run on this return path. */
  call  i16_vfork_restore_frame

  /* Skip the 34-byte vfork-save slot reserved by trap.S syscall entry.
   * Every saved kernel SP (for a process suspended via the syscall path)
   * points at the post-subw position; we adjust here so the popws read
   * the user_SP/user_SS slot at kernel_sp - 4. */
  addw  $34, %sp

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

/* -----------------------------------------------------------------------------
 * i16_ctx_switch — cooperative context switch from kernel thread context.
 *
 * Called as a plain C function.  Saves the current kernel state in the same
 * frame layout as i16_timer_isr / i16_syscall_isr (6-byte IRET frame + 18-byte
 * GP save + 4-byte user_SS/SP slot + 34-byte vfork reserve) so the saved SP
 * can be restored by the shared tail at i16_trap_after_switch without a
 * per-frame format tag.
 *
 * No timer-ISR involvement: this is a synchronous yield, so we build the IRET
 * frame by hand (pushfw + pushw %cs + pushw <return_ip>) instead of relying on
 * INT to push it.  Interrupts are disabled across the SP save / sched_next /
 * SP load critical section; IRET in the restore tail reloads FLAGS (including
 * IF) from the saved frame, so the caller's IF state is preserved.
 *
 * cdecl caller expectations:
 *   - BP, SI, DI, BX, DS, ES preserved (captured in the GP save frame).
 *   - AX, CX, DX are caller-saved; we don't promise to preserve them.
 * ---------------------------------------------------------------------------- */
  .global i16_ctx_switch
i16_ctx_switch:
  /* The caller pushed our return IP.  Lift it out so the fake IRET frame
   * we build below has it in the right slot. */
  popw  %ax                 /* AX = caller return IP */

  /* Build the fake IRET frame: FLAGS, CS, IP.  pushfw snapshots the caller's
   * IF state before we disable interrupts. */
  pushfw                    /* FLAGS (caller's) */
  pushw %cs                 /* CS  = kernel CS */
  pushw %ax                 /* IP  = caller return IP */

  /* Enter critical section: no timer preemption while we swap PCB.sp. */
  cli

  /* Push GP registers in the same order as timer/syscall ISR.
   * The saved AX value is our caller's return IP — harmless, AX is
   * caller-saved in cdecl. */
  pushw %ax
  pushw %bx
  pushw %cx
  pushw %dx
  pushw %si
  pushw %di
  pushw %bp
  pushw %ds
  pushw %es

  /* user_SS = 0 (kernel), user_SP = SP right now (top of GP frame).
   * For a kernel-originated yield, the restore tail's "pop GP, IRET"
   * runs against this same kernel stack — the user_SP slot just tells
   * it where the GP frame lives. */
  movw  %sp, %bp
  xorw  %ax, %ax
  pushw %ax                 /* user_SS */
  pushw %bp                 /* user_SP */

  /* 34-byte vfork-save reserve — matches the other save paths. */
  subw  $34, %sp

  /* Kernel DS/ES for sched_next call (we pushed the caller's above). */
  xorw  %ax, %ax
  movw  %ax, %ds
  movw  %ax, %es

  /* Save our kernel SP into the current PCB (offset 0). */
  movw  %ss:current_core, %bx
  movw  %sp, (%bx)

  /* Shadow the four entry-stub globals into the outgoing PCB — see
   * the matching block in i16_timer_isr for the full rationale. */
  movw  %ss:saved_cs,     %ax
  movw  %ax, 10(%bx)
  movw  %ss:saved_ip,     %ax
  movw  %ax, 12(%bx)
  movw  %ss:vfs_saved_cs, %ax
  movw  %ax, 14(%bx)
  movw  %ss:vfs_saved_ip, %ax
  movw  %ax, 16(%bx)

  /* Pick next runnable process. */
  call  sched_next

  /* current_core = next PCB; load its saved kernel SP and kstack_top. */
  movw  %ax, %ss:current_core
  movw  %ax, %bx
  movw  (%bx), %sp

  /* Restore the incoming PCB's stub-save shadow into the globals. */
  movw  10(%bx), %ax
  movw  %ax, %ss:saved_cs
  movw  12(%bx), %ax
  movw  %ax, %ss:saved_ip
  movw  14(%bx), %ax
  movw  %ax, %ss:vfs_saved_cs
  movw  16(%bx), %ax
  movw  %ax, %ss:vfs_saved_ip

  movw  4(%bx), %ax
  movw  %ax, %ss:i16_current_ksp

  /* Hand off to the shared restore tail.  signal_check on the way
   * out is gated on user_SS != 0, so kernel-mode frames saved by
   * this yield path do not get a user-mode signal delivery frame
   * planted onto the kernel stack. */
  jmp   i16_trap_after_switch
