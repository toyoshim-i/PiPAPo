/*
 * signal.c — ARM Cortex-M signal delivery
 *
 * RTE-based, sa_restorer style.  signal_check pushes a new HW exception
 * frame below PSP with LR = current->sig_restorers[sig].  The CPU unwinds
 * into the handler; `bx lr` lands on the restorer, which issues
 * SYS_RT_SIGRETURN.  sys_rt_sigreturn advances PSP past the sig-delivery
 * frame.
 *
 * Frame layout — see signal_setup_frame below for the per-profile detail.
 */

#include "kernel/core/signal/signal.h"

#include <stddef.h>

#include "kernel/common/spinlock.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/signal/signal_helper.h"
#include "kernel/core/syscall/syscall.h"

/*
 * Build a signal delivery frame on the user stack.
 *
 * ARMv6-M (no FPU):
 *   [PSP - 32]  new 8-word HW frame (signal handler)
 *   [PSP]       original 8-word HW frame (sigframe)
 *   PSP set to PSP - 32.
 *
 * ARMv8-M (Cortex-M33, FPU):
 *   [PSP - 40]  new 8-word basic HW frame (signal handler)
 *   [PSP - 8]   saved EXC_RETURN (4 bytes) + padding (4 bytes)
 *   [PSP]       original HW frame (8 or 26 words depending on FPU)
 *   PSP set to PSP - 40.
 *   svc_exc_return forced to basic frame (bit 4 = 1).
 *
 * On exception return the CPU pops the new frame and runs the handler.
 * The handler's `bx lr` reaches the per-process sa_restorer the user
 * registered via rt_sigaction (typically _ppap_sigreturn_trampoline in
 * src/arch/arm_m/user/syscall.S), which issues SYS_RT_SIGRETURN.
 * sys_rt_sigreturn reverses this: skips the sigreturn SVC frame, reads the
 * saved EXC_RETURN, and restores PSP to the original HW frame.
 */
static int signal_setup_frame(int sig, sighandler_t handler) {
  uint32_t psp;
  uint32_t restorer;
  __asm volatile("mrs %0, psp" : "=r"(psp));

  restorer = (uint32_t)(uintptr_t)current->sig_restorers[sig];
  if (restorer == 0u)
    return -1; /* no sa_restorer registered — cannot deliver safely */

#if __ARM_ARCH >= 8
  /* 8-word basic frame + 2-word saved EXC_RETURN slot = 40 bytes */
  uint32_t new_psp = psp - 40;
#else
  uint32_t new_psp = psp - 32;
#endif

  /* Bounds check: don't write below the stack page */
  uint32_t stack_base =
      (uint32_t)(uintptr_t)mem_region_page_to_ptr(current->stack_page_id);
  if (new_psp < stack_base)
    return -1; /* stack overflow — cannot deliver signal */

  uint32_t *frame = (uint32_t *)new_psp;

  frame[0] = (uint32_t)sig; /* r0 = signal number  */
  frame[1] = 0;             /* r1                  */
  frame[2] = 0;             /* r2                  */
  frame[3] = 0;             /* r3                  */
  frame[4] = 0;             /* r12                 */
  frame[5] = restorer;      /* lr = sa_restorer (Thumb bit set by linker) */
  frame[6] = (uint32_t)(uintptr_t)handler & ~1u; /* pc (bit0 clear)     */
  frame[7] = 0x01000000u;                        /* xpsr (Thumb bit)    */

#if __ARM_ARCH >= 8
  /* Save original EXC_RETURN between signal frame and original HW frame.
   * sys_rt_sigreturn reads this to restore the correct frame type. */
  uint32_t cid = core_id();
  frame[8] = svc_exc_return[cid]; /* saved EXC_RETURN */
  frame[9] = 0;                   /* padding          */

  /* Force SVC_Handler to return with basic frame (bit 4 = 1).
   * The signal handler starts without FPU context. */
  svc_exc_return[cid] = svc_exc_return[cid] | 0x10u;
#endif

  __asm volatile("msr psp, %0" ::"r"(new_psp));
  return 0;
}

void signal_check(void) {
  int sig;
  if (!signal_pop_pending(&sig, NULL)) return;

  /* User handler — set up trampoline frame on user stack */
  sighandler_t handler = current->sig_handlers[sig];
  if (signal_setup_frame(sig, handler) < 0) {
    /* Stack overflow — cannot deliver signal, terminate process */
    sys_exit(128 + sig);
  }
}

/*
 * Restore context after a signal handler returns via sigreturn_trampoline.
 *
 * ARMv6-M: PSP points to the sigreturn SVC's 8-word HW frame.
 *   Skip 32 bytes → original HW frame.
 *
 * ARMv8-M (M33, FPU): PSP points to the sigreturn SVC's HW frame,
 *   which may be basic (32 bytes) or extended (104 bytes) depending on
 *   whether the signal handler used FPU.  After skipping that frame,
 *   we read the saved EXC_RETURN (8 bytes), then PSP = original frame.
 *   We also restore svc_exc_return so SVC_Handler returns with the
 *   correct EXC_RETURN for the original frame type.
 */
long sys_rt_sigreturn(void) {
  uint32_t psp;
  __asm volatile("mrs %0, psp" : "=r"(psp));

#if __ARM_ARCH >= 8
  uint32_t cid = core_id();
  uint32_t exc_ret = svc_exc_return[cid];

  /* Skip the sigreturn SVC's HW frame (basic or extended) */
  uint32_t frame_size = (exc_ret & 0x10u) ? 32u : 104u;
  psp += frame_size;

  /* Read saved EXC_RETURN and skip the 8-byte save slot */
  uint32_t *saved = (uint32_t *)psp;
  uint32_t orig_exc = saved[0];
  psp += 8;

  /* Restore original EXC_RETURN so SVC_Handler returns correctly */
  svc_exc_return[cid] = orig_exc;
#else
  psp += 32;
#endif

  __asm volatile("msr psp, %0" ::"r"(psp));
  return 0; /* value ignored — sigframe[0] has original r0 */
}
