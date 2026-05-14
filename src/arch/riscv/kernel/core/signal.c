/*
 * signal.c — RISC-V signal delivery
 *
 * mret-based, sa_restorer style.  signal_check plants a copy of the
 * interrupted trap frame on the user stack and rewrites the live frame
 * so the trap-exit mret lands in the handler with the right ABI state.
 * The handler returns via `ret` → restorer → ecall SYS_RT_SIGRETURN.
 * sys_rt_sigreturn copies the saved frame back over the live trap
 * frame, restoring every caller-saved register, mepc, mstatus, and
 * user_sp to their pre-signal values.
 *
 * Trap frame layout (144 bytes, see src/arch/riscv/kernel/core/trap.S):
 *
 *   offset 0   x1   (ra)       regs[0]
 *   offset 4   x3   (gp)
 *   offset 8   x4   (tp)
 *   offset 12  x5   (t0)
 *   offset 16  x6   (t1)
 *   offset 20  x7   (t2)
 *   offset 24  x8   (s0)
 *   offset 28  x9   (s1)
 *   offset 32  x10  (a0)       regs[8]
 *   ...
 *   offset 60  x17  (a7)
 *   ...
 *   offset 120 mepc             regs[30]
 *   offset 124 mstatus          regs[31]
 *   offset 128 user_sp          regs[32]
 *
 * signal_check sets:
 *   regs[mepc]    = handler
 *   regs[a0]      = sig         (first arg per RISC-V ABI)
 *   regs[ra]      = sa_restorer (handler's `ret` jumps here)
 *   regs[user_sp] = new_sp      (points just above the saved frame)
 */

#include "kernel/core/signal/signal.h"

#include <stdint.h>

#include "common/errno.h"
#include "kernel/core/arch.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/signal/signal_helper.h"
#include "kernel/core/syscall/syscall.h"

#define RV32_TF_RA_IDX 0u
#define RV32_TF_A0_IDX 8u
#define RV32_TF_MEPC_IDX 30u
#define RV32_TF_USER_SP_IDX 32u
#define RV32_TRAP_FRAME_SIZE 144u /* matches TRAP_FRAME_SIZE in trap.S */

void signal_check(uint32_t *regs) {
  int sig;
  if (!signal_pop_pending(&sig, regs)) return;

  sighandler_t handler = current->sig_handlers[sig];
  uint32_t restorer = (uint32_t)(uintptr_t)current->sig_restorers[sig];
  if (restorer == 0u) {
    sys_exit(128 + sig);
    return;
  }

  uint32_t orig_sp = regs[RV32_TF_USER_SP_IDX];
  uint32_t new_sp = orig_sp - RV32_TRAP_FRAME_SIZE;
  /* RISC-V ABI requires 16-byte sp alignment at function entry.  The
   * trap frame size is already 16-byte aligned (144), so new_sp keeps
   * the alignment of orig_sp. */

  /* Save the interrupted frame verbatim on the user stack.  On
   * sys_rt_sigreturn we copy it back into the then-live trap frame,
   * which restores every caller-saved register to its pre-signal
   * value (callee-saved regs are preserved by the handler's ABI
   * compliance, just like ARM). */
  sys_copy_to_user((uintptr_t)new_sp, regs, RV32_TRAP_FRAME_SIZE);

  /* Rewrite live frame so mret enters the handler in user mode. */
  regs[RV32_TF_MEPC_IDX] = (uint32_t)(uintptr_t)handler;
  regs[RV32_TF_A0_IDX] = (uint32_t)sig;
  regs[RV32_TF_RA_IDX] = restorer;
  regs[RV32_TF_USER_SP_IDX] = new_sp;
}

/*
 * Unwind the delivery frame planted by signal_check.
 *
 * At entry we are inside the restorer's ecall trap.  rv32_trap_frame_sp
 * points at the live trap frame; current user sp (in that frame) equals
 * new_sp — handler's `ret` doesn't touch sp and the restorer hasn't
 * adjusted it either.  The saved pre-signal frame sits at [new_sp ..
 * new_sp + 144).
 *
 * Copying it back over the live trap frame restores every caller-saved
 * register, mepc, mstatus, and user_sp to their pre-signal values.  We
 * then return orig_a0 so syscall_dispatch writes it into the a0 slot,
 * matching what trap-exit would restore anyway (no net change).
 */
long sys_rt_sigreturn(void) {
  uint32_t *regs;
  uint32_t saved_sp;
  uint32_t orig_a0;

  if (rv32_trap_frame_sp == 0u) return -(long)EFAULT;

  regs = (uint32_t *)(uintptr_t)rv32_trap_frame_sp;
  saved_sp = regs[RV32_TF_USER_SP_IDX];
  sys_copy_from_user(regs, (uintptr_t)saved_sp, RV32_TRAP_FRAME_SIZE);
  orig_a0 = regs[RV32_TF_A0_IDX];
  return (long)(int32_t)orig_a0;
}
