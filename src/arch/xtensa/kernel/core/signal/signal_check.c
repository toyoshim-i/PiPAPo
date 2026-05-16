/*
 * signal_check.c — Xtensa LX7 signal delivery
 *
 * call0-ABI, sa_restorer style.  signal_check copies the live trap
 * frame onto the user stack and rewrites the live frame so the rfe
 * out of xtensa_syscall_body enters the handler with the right ABI
 * state.  The handler's terminal `ret` jumps to a0 == sa_restorer
 * (set by sigaction), which issues SYS_RT_SIGRETURN.  sys_rt_sigreturn
 * copies the saved frame back over the live trap frame, restoring
 * pc / ps / a0..a15 / a1 to their pre-signal values.
 *
 * Call0 ABI (PPAP user-space is built -mabi=call0):
 *   a0 = return address     a1 = stack pointer
 *   a2 = first arg / return value
 *   a3 = second arg
 *   a4..a7 = additional args, caller-saved
 *   a12..a15 = callee-saved
 */

#include "kernel/core/signal/signal_check.h"

#include <stdint.h>

#include "common/errno.h"
#include "kernel/core/arch.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/signal/signal.h"
#include "kernel/core/syscall/syscall.h"

void signal_check(XtExcFrame *frame) {
  int sig;
  if (!signal_pop_pending(&sig, (uint32_t *)frame)) return;

  sighandler_t handler = current->sig_handlers[sig];
  uint32_t restorer = (uint32_t)(uintptr_t)current->sig_restorers[sig];
  if (restorer == 0u) {
    /* SIG_DFL / no restorer — terminate the process. */
    sys_exit(128 + sig);
    return;
  }

  /* Reserve space on the user stack for the saved frame.  XtExcFrame
   * is ~100 B; align the new sp to 16 B (call0 ABI requirement). */
  uint32_t orig_sp = (uint32_t)frame->a1;
  uint32_t frame_bytes = (uint32_t)((sizeof(XtExcFrame) + 15u) & ~15u);
  uint32_t new_sp = (orig_sp - frame_bytes) & ~15u;

  /* Save the interrupted frame verbatim on the user stack.  On
   * sys_rt_sigreturn we copy it back into the then-live frame, which
   * restores every caller-saved register, pc, ps, and the original
   * user sp to their pre-signal values. */
  sys_copy_to_user((uintptr_t)new_sp, frame, sizeof(XtExcFrame));

  /* Rewrite the live frame so rfe enters the handler in user mode. */
  frame->pc = (uint32_t)(uintptr_t)handler;
  frame->a2 = (uint32_t)sig; /* first argument: signal number */
  frame->a0 = restorer;      /* handler's terminal `ret` jumps here */
  frame->a1 = new_sp;        /* fresh stack pointer below the saved frame */
}

/*
 * Unwind the delivery frame planted by signal_check.
 *
 * At entry we are inside the restorer's SYS_RT_SIGRETURN trap.
 * xtensa_trap_frame_sp points at the live trap frame; the current
 * user sp (frame->a1) equals new_sp — the restorer hasn't touched it,
 * and call0 handler `ret` doesn't either.  The saved pre-signal frame
 * sits at [new_sp .. new_sp + sizeof(XtExcFrame)).
 *
 * Copying it back over the live trap frame restores every caller-
 * saved register, pc, ps, and the original user sp.  Return the
 * pre-signal a2 so xtensa_syscall_body's return-value write
 * (frame[0] = ret) doesn't overwrite the restored register.
 */
long sys_rt_sigreturn(void) {
  if (xtensa_trap_frame_sp == 0u) return -(long)EFAULT;
  XtExcFrame *frame = (XtExcFrame *)(uintptr_t)xtensa_trap_frame_sp;
  uint32_t saved_sp = (uint32_t)frame->a1;
  sys_copy_from_user(frame, (uintptr_t)saved_sp, sizeof(XtExcFrame));
  return (long)(int32_t)frame->a2;
}
