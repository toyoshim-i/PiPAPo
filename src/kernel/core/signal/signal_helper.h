/*
 * signal_helper.h — internal contract between shared signal core and the
 *                   per-arch signal_check / sys_rt_sigreturn implementations.
 *
 * Included only by:
 *   - src/kernel/core/signal/signal.c   (shared core)
 *   - src/arch/<arch>/kernel/core/signal.c
 *
 * Not a public kernel API.  Anything callable from drivers / VFS / subsys
 * lives in kernel/core/signal/signal.h.
 */

#ifndef PPAP_KERNEL_CORE_SIGNAL_SIGNAL_HELPER_H
#define PPAP_KERNEL_CORE_SIGNAL_SIGNAL_HELPER_H

#include <stdint.h>

#include "kernel/core/signal/signal.h"

/*
 * Hand-rolled count-trailing-zeros for uint32_t.
 *
 * Inlined into every arch's signal_check to pop the lowest pending signal.
 * Hand-rolled (not __builtin_ctz) to avoid pulling in libgcc's __ctzsi2 on
 * targets without a single-instruction CTZ — Cortex-M0+, m68k, RV32 without
 * Zbb, ia16.  Distro libgcc cross builds use flags PPAP cannot rely on
 * (no-PIC, wrong CPU baseline, wrong multilib, etc.).
 */
static inline int ctz32(uint32_t x) {
  int n = 0;
  if (!(x & 0xFFFF)) {
    n += 16;
    x >>= 16;
  }
  if (!(x & 0xFF)) {
    n += 8;
    x >>= 8;
  }
  if (!(x & 0xF)) {
    n += 4;
    x >>= 4;
  }
  if (!(x & 0x3)) {
    n += 2;
    x >>= 2;
  }
  if (!(x & 0x1)) {
    n += 1;
  }
  return n;
}

/*
 * Apply the default disposition for `sig`:
 *   - SIGCHLD: ignore (returns 1).
 *   - Subsystem-handled (Human68k / CP/M / S-OS): defer to subsys op
 *     (returns 1 if the subsys handled it).
 *   - Otherwise: terminate the process via sys_exit(128 + sig); does not
 *     return.
 *
 * Returns 1 when the signal was consumed without taking the user handler
 * path.  `regs` is the saved-register frame for arches that pass one
 * (m68k / riscv); NULL on arches that don't (arm_m / ia16 / xtensa).
 */
int signal_default_action(int sig, uint32_t *regs);

/*
 * Pop one deliverable pending signal and apply the trivial dispositions
 * (SIG_IGN discard, SIG_DFL via signal_default_action).
 *
 * Intended to run at the top of every per-arch signal_check.  Collapses
 * the four-arch preamble (state guard / mask / ctz / clear-bit / IGN /
 * DFL) into one call.
 *
 * Returns:
 *   0 — no further action.  Either no signal was deliverable, the
 *       handler was SIG_IGN (consumed, discard), or SIG_DFL ran
 *       (subsys hook handled it, or sys_exit did not return).
 *   1 — `*sig_out` holds the popped signal number and the handler at
 *       current->sig_handlers[*sig_out] is a user function pointer.
 *       Caller builds the arch-specific delivery frame.
 *
 * `regs` is forwarded to signal_default_action for arches that pass
 * a saved-register frame (m68k / riscv).  Pass NULL on arches that
 * don't (arm_m / ia16 / xtensa).
 */
int signal_pop_pending(int *sig_out, uint32_t *regs);

#endif /* PPAP_KERNEL_CORE_SIGNAL_SIGNAL_HELPER_H */
