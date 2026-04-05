/*
 * irq.h -- IRQ and preemption control for Xtensa LX7 (ESP32-S3)
 *
 * Architecture-specific inline functions for interrupt save/restore and
 * preemption control.  Shared by kernel/common (spinlock.h) and
 * kernel/vfs (klog.c) without requiring kernel/core/arch.h.
 */

#ifndef PPAP_ARCH_XTENSA_KERNEL_COMMON_IRQ_H
#define PPAP_ARCH_XTENSA_KERNEL_COMMON_IRQ_H

#include <stdint.h>

#include "kernel/common/ioregs.h" /* PS_INTLEVEL_MASK, XTENSA_TIMER0_INTMASK */

/* -- Interrupt save / restore ---------------------------------------------
 *
 * Xtensa uses PS.INTLEVEL (bits 3:0) to mask interrupts.  INTLEVEL=0
 * enables all; INTLEVEL=15 disables all.  The RSIL instruction atomically
 * reads the old PS and sets INTLEVEL in one step.
 */

static inline uint32_t arch_irq_save(void) {
  uint32_t old_ps;
  __asm__ volatile("rsil %0, 15" : "=a"(old_ps)); /* INTLEVEL=15 */
  return old_ps;
}

static inline void arch_irq_restore(uint32_t saved) {
  __asm__ volatile("wsr %0, ps\n\t"
                   "rsync" ::"a"(saved));
}

/* -- Interrupt enable / disable ------------------------------------------- */

static inline void arch_irq_enable(void) {
  uint32_t ps;
  __asm__ volatile("rsr %0, ps" : "=a"(ps));
  ps &= ~PS_INTLEVEL_MASK; /* INTLEVEL=0: all interrupts enabled */
  __asm__ volatile("wsr %0, ps\n\t"
                   "rsync" ::"a"(ps));
}

static inline void arch_irq_disable(void) {
  uint32_t dummy;
  __asm__ volatile("rsil %0, 15" : "=a"(dummy)); /* INTLEVEL=15 */
}

/* -- Preemption control ---------------------------------------------------
 *
 * Toggle the timer interrupt (CCOMPARE0) in the INTENABLE register so
 * that only the preemption timer is affected, leaving other interrupts
 * active.
 *
 * xtensa_timer_ready is set by xtensa_timer_init().  Before that,
 * arch_preempt_enable() is a no-op to avoid enabling a stale CCOMPARE0
 * interrupt that has no registered handler yet.
 */

extern volatile uint32_t xtensa_timer_ready;

static inline void arch_preempt_disable(void) {
  uint32_t intenable;
  __asm__ volatile("rsr %0, intenable" : "=a"(intenable));
  intenable &= ~XTENSA_TIMER0_INTMASK;
  __asm__ volatile("wsr %0, intenable\n\t"
                   "rsync" ::"a"(intenable));
}

static inline void arch_preempt_enable(void) {
  if (!xtensa_timer_ready) return; /* timer not initialized yet */
  uint32_t intenable;
  __asm__ volatile("rsr %0, intenable" : "=a"(intenable));
  intenable |= XTENSA_TIMER0_INTMASK;
  __asm__ volatile("wsr %0, intenable\n\t"
                   "rsync" ::"a"(intenable));
}

#endif /* PPAP_ARCH_XTENSA_KERNEL_COMMON_IRQ_H */
