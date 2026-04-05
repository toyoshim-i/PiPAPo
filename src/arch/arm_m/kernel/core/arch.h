/*
 * arch.h — Architecture abstraction for ARM Cortex-M (M-profile)
 *
 * Provides inline functions for architecture-specific operations used by
 * the kernel: interrupt save/restore, yield, barriers, etc.
 *
 * Future architectures (m68k, arm_a) will provide their own arch.h with
 * the same API but different implementations.
 */

#ifndef PPAP_ARCH_ARM_M_KERNEL_CORE_ARCH_H
#define PPAP_ARCH_ARM_M_KERNEL_CORE_ARCH_H

#include <stdint.h>

#include "kernel/common/ioregs.h" /* SCB_ICSR, PENDSVSET */
#include "kernel/common/irq.h"
#include "kernel/core/mm/mem_region.h"

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

static inline page_id_t arch_user_ptr_to_page(page_id_t base_page,
                                              uintptr_t user_ptr,
                                              uint16_t *off) {
  (void)base_page;
  *off = (uint16_t)((uintptr_t)user_ptr & (PAGE_SIZE - 1u));
  return mem_region_ptr_to_page((void *)(uintptr_t)user_ptr);
}

#endif /* PPAP_ARCH_ARM_M_KERNEL_CORE_ARCH_H */
