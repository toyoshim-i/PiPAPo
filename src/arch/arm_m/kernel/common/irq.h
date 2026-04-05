/*
 * irq.h -- IRQ and preemption control for ARM Cortex-M (M-profile)
 *
 * Architecture-specific inline functions for interrupt save/restore and
 * preemption control.  Shared by kernel/common (spinlock.h) and
 * kernel/vfs (klog.c) without requiring kernel/core/arch.h.
 */

#ifndef PPAP_ARCH_ARM_M_KERNEL_COMMON_IRQ_H
#define PPAP_ARCH_ARM_M_KERNEL_COMMON_IRQ_H

#include <stdint.h>

#include "kernel/common/ioregs.h" /* SYST_CSR, SYST_CSR_TICKINT */

/* -- Interrupt save / restore --------------------------------------------- */

/* Save current interrupt state (PRIMASK) and disable interrupts. */
static inline uint32_t arch_irq_save(void) {
  uint32_t saved;
  __asm__ volatile("mrs %0, primask" : "=r"(saved));
  __asm__ volatile("cpsid i");
  return saved;
}

/* Restore interrupt state saved by arch_irq_save(). */
static inline void arch_irq_restore(uint32_t saved) {
  __asm__ volatile("msr primask, %0" ::"r"(saved));
}

/* -- Interrupt enable / disable ------------------------------------------- */

static inline void arch_irq_enable(void) { __asm__ volatile("cpsie i"); }

static inline void arch_irq_disable(void) { __asm__ volatile("cpsid i"); }

/* -- Preemption control ---------------------------------------------------
 *
 * Disable/enable the preemption timer only (SysTick TICKINT).
 * Other interrupts (UART, etc.) remain active.  Used by klog to hold
 * the UART spinlock without blocking ISR-driven TX ring drain.
 */

static inline void arch_preempt_disable(void) { SYST_CSR &= ~SYST_CSR_TICKINT; }

static inline void arch_preempt_enable(void) { SYST_CSR |= SYST_CSR_TICKINT; }

#endif /* PPAP_ARCH_ARM_M_KERNEL_COMMON_IRQ_H */
