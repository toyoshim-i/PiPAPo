/*
 * arch_yield_default.h — shared flag-based arch_yield()
 *
 * Most non-Cortex-M architectures implement context-switch yielding via a
 * single global flag (`switch_pending`) that is set by arch_yield() and
 * checked by the timer ISR / trap-return path.  This header provides the
 * default implementation; per-arch arch.h includes it and gets arch_yield()
 * for free.
 *
 * Architectures that do not use a software flag (e.g. ARM Cortex-M, where
 * PendSV self-pends via NVIC) define ARCH_HAS_YIELD before including this
 * header and provide their own arch_yield() inline above the include.
 *
 * The variable itself is defined in src/kernel/core/proc/sched.c.
 */

#ifndef PPAP_KERNEL_COMMON_ARCH_YIELD_DEFAULT_H
#define PPAP_KERNEL_COMMON_ARCH_YIELD_DEFAULT_H

/* `unsigned int` so storage width = platform word size (16b on i16, 32b
 * elsewhere); per-arch asm reads/writes it with its natural-width
 * instructions (movw on i16, tst.l/clr.l on m68k, lw/sw on riscv). */
extern volatile unsigned int switch_pending;

#ifndef ARCH_HAS_YIELD
static inline void arch_yield(void) { switch_pending = 1; }
#endif

/* Idle-loop drain: returns 1 if a switch is pending and clears the flag.
 * The idle loop then calls sched_switch() so wakeups raised during
 * sched_idle_poll() (e.g. tty_rx_notify from a polled-input target)
 * are honored without waiting for the next timer tick.  ARM Cortex-M
 * defines ARCH_HAS_YIELD_CONSUME and provides its own returning 0
 * because PendSV self-pends via NVIC. */
#ifndef ARCH_HAS_YIELD_CONSUME
static inline int arch_yield_consume(void) {
  if (switch_pending) {
    switch_pending = 0;
    return 1;
  }
  return 0;
}
#endif

#endif /* PPAP_KERNEL_COMMON_ARCH_YIELD_DEFAULT_H */
