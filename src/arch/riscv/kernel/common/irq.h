/*
 * irq.h -- IRQ and preemption control for RISC-V (RV32IMAC)
 *
 * Architecture-specific inline functions for interrupt save/restore and
 * preemption control.  Shared by kernel/common (spinlock.h) and
 * kernel/vfs (klog.c) without requiring kernel/core/arch.h.
 */

#ifndef PPAP_ARCH_RISCV_KERNEL_COMMON_IRQ_H
#define PPAP_ARCH_RISCV_KERNEL_COMMON_IRQ_H

#include <stdint.h>

#include "kernel/common/ioregs.h" /* MSTATUS_MIE, MIE_MTIE */

/* -- Interrupt save / restore ---------------------------------------------
 *
 * RISC-V uses mstatus.MIE (bit 3) to globally enable/disable M-mode
 * interrupts.  We save the MIE bit and clear it atomically with csrrc.
 */

static inline uint32_t arch_irq_save(void) {
  uint32_t mstatus;
  __asm__ volatile("csrrc %0, mstatus, %1" : "=r"(mstatus) : "r"(MSTATUS_MIE));
  return mstatus & MSTATUS_MIE;
}

static inline void arch_irq_restore(uint32_t saved) {
  if (saved) __asm__ volatile("csrs mstatus, %0" ::"r"(MSTATUS_MIE));
}

/* -- Interrupt enable / disable ------------------------------------------- */

static inline void arch_irq_enable(void) {
  __asm__ volatile("csrs mstatus, %0" ::"r"(MSTATUS_MIE));
}

static inline void arch_irq_disable(void) {
  __asm__ volatile("csrc mstatus, %0" ::"r"(MSTATUS_MIE));
}

/* -- Preemption control ---------------------------------------------------
 *
 * Toggle the Machine Timer Interrupt Enable (mie.MTIE) so that only
 * the preemption timer is affected, leaving other interrupts active.
 */

static inline void arch_preempt_disable(void) {
  __asm__ volatile("csrc mie, %0" ::"r"(MIE_MTIE));
}

static inline void arch_preempt_enable(void) {
  __asm__ volatile("csrs mie, %0" ::"r"(MIE_MTIE));
}

#endif /* PPAP_ARCH_RISCV_KERNEL_COMMON_IRQ_H */
