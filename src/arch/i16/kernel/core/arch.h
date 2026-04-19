/*
 * arch.h -- Architecture abstraction for i8086 (16-bit real mode)
 *
 * Provides the same API as src/arch/arm_m/arch.h, src/arch/m68k/arch.h,
 * and src/arch/riscv/arch.h but with i8086 real-mode implementations.
 *
 * Phase P-1: Stubs only -- just enough to compile the boot banner.
 * Phase P-2 will add real timer/preemption/context-switch support.
 */

#ifndef PPAP_ARCH_I16_KERNEL_CORE_ARCH_H
#define PPAP_ARCH_I16_KERNEL_CORE_ARCH_H

#include <stdint.h>

#include "kernel/common/core/page_types.h"
#include "kernel/common/irq.h"

/* -- Context switch trigger ---------------------------------------------
 *
 * Flag-driven context switch, same pattern as m68k and RISC-V.
 * The timer ISR checks this flag and performs the switch if set.
 * ------------------------------------------------------------------- */

extern volatile uint16_t i16_current_ksp;

/* Shared flag-based yield.  See kernel/common/arch_yield_default.h.
 * Skip the default arch_sched_switch: ia16 has no PendSV equivalent,
 * so a thread-context switch needs i16_sched_yield() directly rather
 * than just setting the flag (nothing would consume it until the next
 * timer tick). */
#define ARCH_HAS_SCHED_SWITCH
#include "kernel/common/arch_yield_default.h"

void i16_sched_yield(void);
static inline void arch_sched_switch(void) {
  switch_pending = 0;
  i16_sched_yield();
}

/* -- Scheduler startup hook ---------------------------------------------
 *
 * Called from the shared sched_start() before arch_irq_enable().
 * Re-plant kernel-stack canaries: during boot the init sequence ran on
 * the boot stack (slot 7 region) and may have overwritten canaries in
 * lower slots.  Now that boot is done, replant them before the first
 * timer tick triggers canary checks. */
void proc_plant_kstack_canaries(void);
static inline void arch_sched_start_hook(void) { proc_plant_kstack_canaries(); }

/* -- CPU hints ---------------------------------------------------------- */

static inline void arch_wfi(void) { __asm__ volatile("hlt"); }

static inline void arch_wfe(void) { arch_wfi(); }
static inline void arch_sev(void) { /* no-op, single core */ }

/* -- Memory barriers ----------------------------------------------------
 *
 * 8086 is single-core with in-order execution.  No barriers needed.
 * ------------------------------------------------------------------- */

static inline void arch_dsb_isb(void) {
  __asm__ volatile("" ::: "memory"); /* Compiler barrier only */
}

static inline page_id_t arch_user_ptr_to_page(page_id_t base_page,
                                              uintptr_t user_ptr,
                                              uint16_t *off) {
  uint32_t user_off = (uint32_t)(uint16_t)user_ptr;

  *off = (uint16_t)(user_off % PAGE_SIZE);
  return base_page + (page_id_t)(user_off / PAGE_SIZE);
}

#endif /* PPAP_ARCH_I16_KERNEL_CORE_ARCH_H */
