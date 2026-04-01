/*
 * arch.h — Architecture abstraction for ARM Cortex-M (M-profile)
 *
 * Provides inline functions for architecture-specific operations used by
 * the kernel: interrupt save/restore, yield, barriers, etc.
 *
 * Future architectures (m68k, arm_a) will provide their own arch.h with
 * the same API but different implementations.
 */

#ifndef PPAP_ARCH_ARM_M_ARCH_H
#define PPAP_ARCH_ARM_M_ARCH_H

#include <stdint.h>
#include "../../kernel/mm/mem_region.h"

#include "ioregs.h" /* SCB_ICSR, PENDSVSET */

/* ── Interrupt save / restore ───────────────────────────────────────────────
 */

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

/* ── Interrupt enable / disable ─────────────────────────────────────────────
 */

static inline void arch_irq_enable(void) { __asm__ volatile("cpsie i"); }

static inline void arch_irq_disable(void) { __asm__ volatile("cpsid i"); }

/* ── Preemption control ──────────────────────────────────────────────────── *
 *
 * Disable/enable the preemption timer only (SysTick TICKINT).
 * Other interrupts (UART, etc.) remain active.  Used by klog to hold
 * the UART spinlock without blocking ISR-driven TX ring drain.
 */

static inline void arch_preempt_disable(void) { SYST_CSR &= ~SYST_CSR_TICKINT; }

static inline void arch_preempt_enable(void) { SYST_CSR |= SYST_CSR_TICKINT; }

/* ── Context switch trigger ─────────────────────────────────────────────────
 */

/* Pend PendSV exception — triggers a context switch at the next opportunity. */
static inline void arch_yield(void) { SCB_ICSR |= PENDSVSET; }

/* ── CPU hints ──────────────────────────────────────────────────────────────
 */

/* Wait for interrupt — puts the CPU in low-power sleep until an IRQ fires. */
static inline void arch_wfi(void) { __asm__ volatile("wfi"); }

/* Wait for event — used for inter-core synchronisation on RP2040. */
static inline void arch_wfe(void) { __asm__ volatile("wfe"); }

/* Send event — wakes a core sleeping in WFE. */
static inline void arch_sev(void) { __asm__ volatile("sev"); }

/* ── Memory barriers ────────────────────────────────────────────────────────
 */

/* Data synchronisation barrier + instruction synchronisation barrier.
 * Used after MPU reprogramming to ensure new settings take effect. */
static inline void arch_dsb_isb(void) {
  __asm__ volatile("dsb\n isb" ::: "memory");
}

/* ── User-space byte read ──────────────────────────────────────────────────
 * ARM SRAM supports byte access — plain dereference is fine. */
static inline uint8_t read_user_byte(const uint8_t *ptr) { return *ptr; }

static inline page_id_t arch_user_ptr_to_page(page_id_t base_page,
                                              uintptr_t user_ptr,
                                              uint16_t *off) {
  (void)base_page;
  *off = (uint16_t)((uintptr_t)user_ptr & (PAGE_SIZE - 1u));
  return mem_region_ptr_to_page((void *)(uintptr_t)user_ptr);
}

#endif /* PPAP_ARCH_ARM_M_ARCH_H */
