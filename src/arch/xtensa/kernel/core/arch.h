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

/* Xtensa vfork builds the child frame on the fixed kstack slot. */
#define ARCH_VFORK_COPY_PROCESS_STACK 0
#define ARCH_VFORK_CHILD_FRAME_POINTER 0

/* Solicited/new-process frame layout used by switch.S. */
#define XTENSA_SOL_EXIT_WORD 4u
#define XTENSA_SOL_PC_WORD 5u
#define XTENSA_SOL_PS_WORD 6u
#define XTENSA_SOL_SP_WORD 7u
#define XTENSA_SOL_A0_WORD 8u
#define XTENSA_SOL_A3_WORD 9u
#define XTENSA_SOL_FRMSZ 64u

uint32_t *xtensa_build_vfork_child_frame(uint32_t kernel_sp, uint32_t child_pc,
                                         uint32_t child_user_sp,
                                         uint32_t child_a0, uint32_t child_a3);

/* ── Context switch trigger ──────────────────────────────────────────────
 *
 * Xtensa has no PendSV equivalent.  We use the RISC-V/m68k pattern:
 * set a flag that the timer ISR checks after calling sched_timer_tick().
 * ────────────────────────────────────────────────────────────────────────── */

/* Shared flag-based yield.  See kernel/common/arch_yield_default.h.
 * Skip the default arch_sched_switch: Xtensa has no PendSV equivalent,
 * so a thread-context switch needs to call xtensa_do_yield() directly
 * rather than just setting the flag. */
#define ARCH_HAS_SCHED_SWITCH
#include "kernel/common/arch_yield_default.h"

void xtensa_do_yield(void);
static inline void arch_sched_switch(void) {
  switch_pending = 0;
  xtensa_do_yield();
}

/* ── Scheduler startup hook ────────────────────────────────────────────────
 *
 * Called from the shared sched_start() before arch_irq_enable().
 * Xtensa: no PSP/MSP split, no PendSV priorities.  Timer ISR setup is done
 * by target_late_init() → xtensa_timer_init(), which runs BEFORE
 * sched_start() and has already set INTENABLE to only the CCOMPARE0 bit.
 * Clear pending interrupts but preserve INTENABLE. */
void xtensa_trap_reassert(void);
static inline void arch_sched_start_hook(void) {
  xtensa_trap_reassert();
  __asm__ volatile("wsr %0, intclear; rsync" ::"r"(0xFFFFFFFFu));
}

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
