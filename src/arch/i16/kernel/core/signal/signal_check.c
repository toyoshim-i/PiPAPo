/*
 * signal_check.c — Intel i8086 real-mode signal delivery
 *
 * IRET-based, split-frame aware.  trap.S passes (user_sp, user_ss) from
 * the kernel-stack slot to signal_check(), which plants a new GP+IRET
 * frame plus a small trailer on the user stack and returns the new
 * user_sp.  Handler's near `ret` lands on the restorer in proc_seg;
 * that stub issues INT 30h SYS_RT_SIGRETURN, which restores the
 * pre-signal user_sp in the kernel slot.
 *
 * Frame layout (all on the user stack, SS=proc_seg):
 *
 *   [new_user_SP + 0..17]   GP register frame  (ES, DS, BP, DI, SI, DX,
 *                           CX, BX, AX — 9 words, popped by trap.S on
 *                           the restore path)
 *   [new_user_SP + 18..23]  IRET frame (IP=handler, CS=proc_seg,
 *                           FLAGS=preserved from original)
 *   [new_user_SP + 24..25]  return IP for handler's near `ret`  ──┐
 *   [new_user_SP + 26..27]  signal number (handler's first arg)  │ trailer
 *   [new_user_SP + 28..29]  saved pre-signal user_SP              ─┘
 *
 * The return IP is the per-handler sa_restorer recorded in
 * current->sig_restorers[sig] by rt_sigaction.  User-space libc/crt
 * supplies the trampoline body — typically `_ppap_sigreturn_trampoline`
 * in src/arch/i16/user/syscall.S — which does
 * `addw $2, %sp; movw $SYS_RT_SIGRETURN, %ax; int $0x30` and lands us
 * in sys_rt_sigreturn with user_SP = new_user_SP + 4 (after trap.S's
 * own 18-byte GP-register push).  sys_rt_sigreturn reads the saved
 * pre-signal user_SP at new_user_SP + 28 and writes it back to the
 * kernel-stack slot so the final IRET resumes at the original
 * pre-signal context.
 */

#include "kernel/core/signal/signal_check.h"

#include <stddef.h>
#include <stdint.h>

#include "common/errno.h"
#include "kernel/common/config.h"
#include "kernel/core/arch.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/signal/signal.h"
#include "kernel/core/syscall/syscall.h"

#define I16_GP_FRAME_BYTES 18u  /* ES/DS/BP/DI/SI/DX/CX/BX/AX  (9 words) */
#define I16_IRET_FRAME_BYTES 6u /* IP/CS/FLAGS                          */
#define I16_FULL_FRAME_BYTES (I16_GP_FRAME_BYTES + I16_IRET_FRAME_BYTES)
#define I16_SIG_TRAILER_BYTES 6u /* ret_ip + sig_arg + saved_user_sp      */
#define I16_SIG_DELIVERY_BYTES (I16_FULL_FRAME_BYTES + I16_SIG_TRAILER_BYTES)

#define I16_USER_SEG_PAGES 16u
#define I16_USER_STACK_PAGE_BASE \
  ((uint16_t)((I16_USER_SEG_PAGES - 1u) * PAGE_SIZE))

uint16_t signal_check(uint16_t user_sp, uint16_t user_ss) {
  int sig;
  sighandler_t handler;
  uint16_t new_sp;
  uint16_t trampoline;
  uint16_t orig_flags;

  if (!signal_pop_pending(&sig, NULL)) return user_sp;

  handler = current->sig_handlers[sig];

  /* User handler — build a delivery frame below the current user_SP.
   * The near-return target is the per-handler sa_restorer registered
   * by rt_sigaction (user-space trampoline supplied by crt0/syscall.S
   * via the _ppap_sigreturn_trampoline symbol).  Treat a missing
   * restorer like stack overflow: we can't land anywhere sane after
   * the handler returns. */
  trampoline = (uint16_t)(uintptr_t)current->sig_restorers[sig];
  if (trampoline == 0u) {
    sys_exit(128 + sig);
    return user_sp;
  }
  if (user_sp < (uint16_t)(I16_USER_STACK_PAGE_BASE + I16_SIG_DELIVERY_BYTES)) {
    sys_exit(128 + sig);
    return user_sp;
  }
  new_sp = (uint16_t)(user_sp - I16_SIG_DELIVERY_BYTES);

  /* All user-stack reads/writes go through the portable sys_copy_*
   * helpers.  On ia16 those resolve the 16-bit near pointer through
   * current->image (proc_seg base + segment offset) and reach into
   * page storage via page_read/write — no arch-specific
   * plumbing needed here. */

  /* Preserve FLAGS from the original IRET frame so the handler runs
   * with the same interrupt-enable state as the interrupted code. */
  sys_copy_from_user(&orig_flags,
                     (uintptr_t)(user_sp + I16_GP_FRAME_BYTES + 4u), 2);

  {
    uint16_t gp_words[9] = {
        user_ss, /* ES */
        user_ss, /* DS */
        0,       /* BP */
        0,       /* DI */
        0,       /* SI */
        0,       /* DX */
        0,       /* CX */
        0,       /* BX */
        0,       /* AX */
    };
    sys_copy_to_user((uintptr_t)new_sp, gp_words, sizeof(gp_words));
  }
  {
    uint16_t iret_words[3];
    iret_words[0] = (uint16_t)(uintptr_t)handler; /* IP */
    iret_words[1] = user_ss;                      /* CS = proc_seg */
    iret_words[2] = orig_flags;                   /* FLAGS */
    sys_copy_to_user((uintptr_t)(new_sp + I16_GP_FRAME_BYTES), iret_words,
                     sizeof(iret_words));
  }
  {
    uint16_t trailer[3];
    trailer[0] = trampoline;    /* handler's near-ret target */
    trailer[1] = (uint16_t)sig; /* handler's first argument  */
    trailer[2] = user_sp;       /* saved pre-signal user_SP  */
    sys_copy_to_user((uintptr_t)(new_sp + I16_FULL_FRAME_BYTES), trailer,
                     sizeof(trailer));
  }

  return new_sp;
}

/*
 * Unwind the sig-delivery frame the kernel planted below the original
 * user_SP.  At entry, the kernel-stack slot holds the post-trap user_SP
 * which sits I16_FULL_FRAME_BYTES (= 24) below the saved pre-signal
 * user_SP trailer word written by signal_check:
 *
 *   [current user_SP + 24]  saved_orig_user_SP  ← read back here
 *
 * Overwriting the kernel slot is sufficient — trap.S's .Lrestore path
 * will then IRET through the pre-signal IRET frame still sitting in
 * place on the user stack.
 */
long sys_rt_sigreturn(void) {
  uint16_t *slot;
  uint16_t user_sp;
  uint16_t saved_sp = 0;

  if (i16_trap_frame_sp == 0u) return -(long)EFAULT;

  slot = (uint16_t *)(uintptr_t)i16_trap_frame_sp;
  user_sp = slot[0];

  sys_copy_from_user(&saved_sp, (uintptr_t)(user_sp + I16_FULL_FRAME_BYTES), 2);
  slot[0] = saved_sp;
  return 0;
}
