/*
 * signal.c — Motorola 68000 signal delivery
 *
 * RTE-based, sa_restorer style.  signal_check rewrites the (SR, PC)
 * slot in the trap frame so rte enters the handler in user mode, and
 * plants a small sig-arg + saved-context trailer below USP.  Handler's
 * `rts` lands on the restorer (which pops the sig arg slot and issues
 * SYS_RT_SIGRETURN).  sys_rt_sigreturn reads the saved (SR, PC, USP)
 * out of the trailer and rewrites the current trap frame so the
 * trap-exit rte resumes at the pre-signal user PC.
 *
 * Trap frame layout (66 B, set up by src/arch/m68k/kernel/core/trap.S):
 *
 *   [regs + 0 .. 56]   d0..d7, a0..a6    (60 B, restored by movem at exit)
 *   [regs + 60]        SR                (2 B, popped by rte)
 *   [regs + 62]        PC                (4 B, popped by rte)
 *
 * Delivery frame on the user stack (78 B, starting at new_usp):
 *
 *   [new_usp + 0 ]  sa_restorer         (4 B — handler's rts target)
 *   [new_usp + 4 ]  sig                 (4 B — handler's first stack arg)
 *   [new_usp + 8 ]  saved_orig_usp      (4 B)
 *   [new_usp + 12]  saved_trap_frame    (66 B — d0..d7/a0..a6/SR/PC)
 *
 * The saved frame holds the pre-signal register state, not just SR/PC.
 * That's required because the handler can clobber any caller-saved reg
 * (d0/d1/a0/a1) and sys_rt_sigreturn needs to restore the pre-signal
 * values — the trap-exit rte reloads the whole register set from the
 * frame.  Handler receives `sig` at 8(%a6) via the standard m68k C ABI
 * (the 4-byte slot at new_usp+4 is the first stack-passed arg).  On
 * return the handler does `rts` (pops sa_restorer); the restorer
 * discards the arg with `addq.l #4,%sp` and issues SYS_RT_SIGRETURN,
 * which unwinds the delivery frame.
 *
 * No signal blocking is performed during the handler — matches the ARM
 * and ia16 branches.
 */

#include "kernel/core/signal/signal.h"

#include <stdint.h>

#include "common/errno.h"
#include "kernel/core/arch.h" /* m68k_trap_frame_sp */
#include "kernel/core/proc/proc.h"
#include "kernel/core/signal/signal_helper.h"
#include "kernel/core/syscall/syscall.h"

#define M68K_TRAP_FRAME_SIZE 66u
#define M68K_TRAP_FRAME_D0_OFF 0u
#define M68K_TRAP_FRAME_SR_OFF 60u
#define M68K_TRAP_FRAME_PC_OFF 62u
#define M68K_SIG_HEADER_BYTES 12u /* restorer + sig + saved_orig_usp */
#define M68K_SIG_DELIVERY_BYTES (M68K_SIG_HEADER_BYTES + M68K_TRAP_FRAME_SIZE)

static inline void m68k_set_usp(uint32_t usp) {
  __asm__ volatile("move.l %0, %%usp" ::"a"(usp));
}

void signal_check(uint32_t *regs) {
  int sig;
  if (!signal_pop_pending(&sig, regs)) return;

  sighandler_t handler = current->sig_handlers[sig];
  uint32_t restorer = (uint32_t)(uintptr_t)current->sig_restorers[sig];
  if (restorer == 0u) {
    sys_exit(128 + sig);
    return;
  }

  uint8_t *frame = (uint8_t *)regs;
  uint16_t orig_sr;
  uint32_t orig_usp = current->usp;
  __builtin_memcpy(&orig_sr, frame + M68K_TRAP_FRAME_SR_OFF, 2);

  uint32_t new_usp = orig_usp - M68K_SIG_DELIVERY_BYTES;
  uint32_t sig32 = (uint32_t)sig;

  sys_copy_to_user((uintptr_t)(new_usp + 0u), &restorer, 4);
  sys_copy_to_user((uintptr_t)(new_usp + 4u), &sig32, 4);
  sys_copy_to_user((uintptr_t)(new_usp + 8u), &orig_usp, 4);
  sys_copy_to_user((uintptr_t)(new_usp + M68K_SIG_HEADER_BYTES), frame,
                   M68K_TRAP_FRAME_SIZE);

  /* Rewrite the trap frame's PC so rte enters the handler in user mode.
   * Preserve the original SR's CCR/IPL bits via the memcpy already done —
   * the hw push captured the pre-trap user SR with S=0, so the trap
   * frame's SR slot is already correct for a user-mode rte. */
  uint32_t new_pc = (uint32_t)(uintptr_t)handler;
  __builtin_memcpy(frame + M68K_TRAP_FRAME_SR_OFF, &orig_sr, 2);
  __builtin_memcpy(frame + M68K_TRAP_FRAME_PC_OFF, &new_pc, 4);

  /* Point the USP at the delivery frame so the handler's stack-passed
   * `sig` argument is found at 8(%a6) / 4(%sp-at-entry). */
  current->usp = new_usp;
  m68k_set_usp(new_usp);
}

/*
 * Unwind the delivery frame planted by signal_check.
 *
 * At entry — after the handler's `rts` and the sa_restorer stub doing
 * `addq.l #4,%sp` + `trap #0` — current->usp points at the saved-context
 * trailer laid out in signal_check:
 *
 *   [usp + 0 ]  saved_orig_usp
 *   [usp + 4 ]  saved_trap_frame  (66 B — d0..d7/a0..a6/SR/PC)
 *
 * Copy the saved frame back over the live trap frame: this restores the
 * pre-signal SR, PC, and every general register to their pre-signal
 * values — the trap-exit movem reloads d0..d7/a0..a6 from the frame
 * wholesale.  Restore USP and return orig_d0 so m68k_syscall_entry's
 * `regs[0] = regs[1]` step lands the pre-signal d0 back into the d0
 * slot (syscall_dispatch writes our return into regs[1] first, then
 * m68k_syscall_entry copies to regs[0]).  d1 ends up equal to d0 in the
 * resulting user-visible state, which matches the m68k syscall ABI —
 * d1 is an argument register, not preserved across a syscall.
 */
long sys_rt_sigreturn(void) {
  uint8_t *frame;
  uintptr_t user_sp;
  uint32_t orig_usp;
  uint8_t saved[M68K_TRAP_FRAME_SIZE];
  uint32_t orig_d0;

  if (m68k_trap_frame_sp == 0u) return -(long)EFAULT;

  user_sp = current->usp;
  sys_copy_from_user(&orig_usp, user_sp + 0u, 4);
  sys_copy_from_user(saved, user_sp + 4u, M68K_TRAP_FRAME_SIZE);

  frame = (uint8_t *)(uintptr_t)m68k_trap_frame_sp;
  __builtin_memcpy(frame, saved, M68K_TRAP_FRAME_SIZE);

  current->usp = orig_usp;
  m68k_set_usp(orig_usp);

  __builtin_memcpy(&orig_d0, saved + M68K_TRAP_FRAME_D0_OFF, 4);
  return (long)(int32_t)orig_d0;
}
