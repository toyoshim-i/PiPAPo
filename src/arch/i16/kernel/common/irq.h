/*
 * irq.h -- IRQ and preemption control for i8086 (16-bit real mode)
 *
 * Architecture-specific inline functions for interrupt save/restore and
 * preemption control.  Shared by kernel/common (spinlock.h) and
 * kernel/vfs (klog.c) without requiring kernel/core/arch.h.
 */

#ifndef PPAP_ARCH_I16_IRQ_H
#define PPAP_ARCH_I16_IRQ_H

#include <stdint.h>

/* -- Interrupt save / restore ---------------------------------------------
 *
 * i8086 uses the FLAGS register IF bit (bit 9) to enable/disable
 * maskable interrupts.  CLI clears IF, STI sets it.
 */

static inline uint16_t arch_irq_save(void) {
  uint16_t flags;
  __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
  return flags & 0x0200u; /* Isolate IF bit */
}

static inline void arch_irq_restore(uint16_t saved) {
  if (saved)
    __asm__ volatile("sti");
}

/* -- Interrupt enable / disable ------------------------------------------- */

static inline void arch_irq_enable(void) { __asm__ volatile("sti"); }

static inline void arch_irq_disable(void) { __asm__ volatile("cli"); }

/* -- Preemption control ---------------------------------------------------
 *
 * TODO: implement PIT IRQ0 mask via 8259A OCW1 for fine-grained
 * preemption control (P-2).  For now, alias to global IRQ toggle.
 */

static inline void arch_preempt_disable(void) { arch_irq_disable(); }

static inline void arch_preempt_enable(void) { arch_irq_enable(); }

#endif /* PPAP_ARCH_I16_IRQ_H */
