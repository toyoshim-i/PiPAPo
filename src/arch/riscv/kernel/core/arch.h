/*
 * arch.h — Architecture abstraction for RISC-V (RV32IMAC)
 *
 * Provides the same API as src/arch/arm_m/arch.h and src/arch/m68k/arch.h
 * but with RISC-V implementations.  Used by shared kernel code (spinlock.h,
 * sched.c, main.c, etc.) for architecture-independent operations.
 */

#ifndef PPAP_ARCH_RISCV_KERNEL_CORE_ARCH_H
#define PPAP_ARCH_RISCV_KERNEL_CORE_ARCH_H

#include <stdint.h>

#include "kernel/common/ioregs.h"
#include "kernel/common/irq.h"
#include "kernel/core/mm/mem_region.h"

/* ── Context switch trigger ──────────────────────────────────────────────
 *
 * RISC-V has no PendSV equivalent.  We use the m68k pattern: set a flag
 * that the timer ISR checks after calling sched_timer_tick().
 * For cooperative yield, ecall from M-mode triggers the switch directly.
 * ────────────────────────────────────────────────────────────────────────── */

/* Shared flag-based yield.  See kernel/common/arch_yield_default.h. */
#include "kernel/common/arch_yield_default.h"

/* ── Scheduler startup hook ────────────────────────────────────────────────
 *
 * Called from the shared sched_start() before arch_irq_enable().
 * Sets mscratch to pid 0's kernel stack top.  boot.S initialized it to
 * __stack_top (linker stack), but now pid 0 has its own stack_page.
 * Must be done before enabling interrupts. */
#include "kernel/common/core/proc_info.h" /* proc_table */
static inline void arch_sched_start_hook(void) {
  uint32_t ksp =
      (uint32_t)(uintptr_t)mem_region_page_to_ptr(proc_table[0].stack_page_id) +
      PAGE_SIZE;
  proc_table[0].kernel_sp = ksp;
  __asm__ volatile("csrw mscratch, %0" : : "r"(ksp));
}

/* ── CPU hints ────────────────────────────────────────────────────────── */

static inline void arch_wfi(void) { __asm__ volatile("wfi"); }

/* WFE: not a standard RISC-V instruction — use WFI as fallback */
static inline void arch_wfe(void) { arch_wfi(); }

/* SEV: on RP2350 RISC-V, inter-core wakeup uses SIO doorbell.
 * For now, stub as no-op (single-core initial port). */
static inline void arch_sev(void) { /* no-op on single-core */ }

/* ── Memory barriers ──────────────────────────────────────────────────── */

static inline void arch_dsb_isb(void) {
  __asm__ volatile("fence rw, rw" ::: "memory");
  __asm__ volatile("fence.i" ::: "memory");
}

static inline page_id_t arch_user_ptr_to_page(page_id_t base_page,
                                              uintptr_t user_ptr,
                                              uint16_t *off) {
  (void)base_page;
  *off = (uint16_t)((uintptr_t)user_ptr & (PAGE_SIZE - 1u));
  return mem_region_ptr_to_page((void *)(uintptr_t)user_ptr);
}

#endif /* PPAP_ARCH_RISCV_KERNEL_CORE_ARCH_H */
