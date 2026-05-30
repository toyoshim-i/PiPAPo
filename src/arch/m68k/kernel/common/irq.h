/*
 * irq.h -- IRQ and preemption control for Motorola 68000
 *
 * Architecture-specific inline functions for interrupt save/restore and
 * preemption control.  Shared by kernel/common (spinlock.h) and
 * kernel/vfs (klog.c) without requiring kernel/core/arch.h.
 */

#ifndef PPAP_ARCH_M68K_KERNEL_COMMON_IRQ_H
#define PPAP_ARCH_M68K_KERNEL_COMMON_IRQ_H

#include <stdint.h>

/* -- Interrupt save / restore ---------------------------------------------
 *
 * 68k uses the SR interrupt priority level (IPL) bits 10-8.
 * Setting IPL to 7 masks all maskable interrupts.
 */

static inline uint32_t arch_irq_save(void) {
  uint16_t saved;
  __asm__ volatile(
      "move.w  %%sr,%0\n"
      "or.w    #0x0700,%%sr\n"
      : "=d"(saved)
      :
      : "cc");
  return (uint32_t)saved;
}

static inline void arch_irq_restore(uint32_t saved) {
  __asm__ volatile("move.w  %0,%%sr\n" : : "d"((uint16_t)saved) : "cc");
}

/* -- Interrupt enable / disable ------------------------------------------- */

static inline void arch_irq_enable(void) {
  __asm__ volatile("and.w   #0xF8FF,%%sr\n" ::: "cc");
}

static inline void arch_irq_disable(void) {
  __asm__ volatile("or.w    #0x0700,%%sr\n" ::: "cc");
}

static inline int arch_in_irq_context(void) {
  uint16_t sr;
  __asm__ volatile("move.w  %%sr,%0\n" : "=d"(sr));
  return (sr & 0x0700u) != 0u;
}

/* -- Preemption control ---------------------------------------------------
 *
 * On single-core 68k, disabling all IRQs is sufficient and safe --
 * putc is synchronous (never blocks), so no deadlock risk.
 */

static inline void arch_preempt_disable(void) { arch_irq_disable(); }

static inline void arch_preempt_enable(void) { arch_irq_enable(); }

#endif /* PPAP_ARCH_M68K_KERNEL_COMMON_IRQ_H */
