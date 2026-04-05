/*
 * arch.h — Architecture abstraction for Xtensa LX7 (ESP32-S3)
 *
 * Provides the same API as src/arch/arm_m/arch.h, src/arch/m68k/arch.h,
 * and src/arch/riscv/arch.h but with Xtensa implementations.
 *
 * Windowed ABI: ESP-IDF requires windowed register ABI.
 * Context switch uses solicited-frame pattern with window spill.
 */

#ifndef PPAP_ARCH_XTENSA_KERNEL_CORE_ARCH_H
#define PPAP_ARCH_XTENSA_KERNEL_CORE_ARCH_H

#include <stdint.h>

#include "kernel/common/ioregs.h"
#include "kernel/common/irq.h"
#include "kernel/core/mm/mem_region.h"

/* ── Context switch trigger ──────────────────────────────────────────────
 *
 * Xtensa has no PendSV equivalent.  We use the RISC-V/m68k pattern:
 * set a flag that the timer ISR checks after calling sched_timer_tick().
 * ────────────────────────────────────────────────────────────────────────── */

extern volatile uint32_t xtensa_switch_pending;

static inline void arch_yield(void) { xtensa_switch_pending = 1; }

/* ── CPU hints ────────────────────────────────────────────────────────── */

static inline void arch_wfi(void) { __asm__ volatile("waiti 0"); }

/* WFE: not a standard Xtensa instruction — use WAITI as fallback */
static inline void arch_wfe(void) { arch_wfi(); }

/* SEV: on ESP32-S3, inter-core wakeup uses crosscore interrupt.
 * For now, stub as no-op (single-core initial port). */
static inline void arch_sev(void) { /* no-op on single-core */ }

/* ── Memory barriers ──────────────────────────────────────────────────── */

static inline void arch_dsb_isb(void) {
  __asm__ volatile("memw" ::: "memory");  /* data memory barrier */
  __asm__ volatile("isync" ::: "memory"); /* instruction sync */
}

static inline page_id_t arch_user_ptr_to_page(page_id_t base_page,
                                              uintptr_t user_ptr,
                                              uint16_t *off) {
  (void)base_page;
  *off = (uint16_t)((uintptr_t)user_ptr & (PAGE_SIZE - 1u));
  return mem_region_ptr_to_page((void *)(uintptr_t)user_ptr);
}

#endif /* PPAP_ARCH_XTENSA_KERNEL_CORE_ARCH_H */
