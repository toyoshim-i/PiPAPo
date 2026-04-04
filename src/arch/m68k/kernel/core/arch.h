/*
 * arch.h — Architecture abstraction for Motorola 68000
 *
 * Provides the same API as src/arch/arm_m/arch.h but with 68k
 * implementations.  Used by shared kernel code (spinlock.h,
 * sched.c, main.c, etc.) for architecture-independent operations.
 */

#ifndef PPAP_ARCH_M68K_ARCH_H
#define PPAP_ARCH_M68K_ARCH_H

#include <stdint.h>
#include "core/mm/mem_region.h"

/* ── Interrupt save / restore ─────────────────────────────────────────────
 *
 * 68k uses the SR interrupt priority level (IPL) bits 10-8.
 * Setting IPL to 7 masks all maskable interrupts.
 * We save the full SR and restore it to get back the original IPL.
 * ────────────────────────────────────────────────────────────────────────── */

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

/* ── Interrupt enable / disable ───────────────────────────────────────────
 *
 * Enable: set IPL to 0 (supervisor mode, all IRQs enabled).
 * Disable: set IPL to 7 (mask all maskable IRQs).
 * Both preserve the S bit (stay in supervisor mode).
 * ────────────────────────────────────────────────────────────────────────── */

static inline void arch_irq_enable(void) {
  __asm__ volatile("and.w   #0xF8FF,%%sr\n" /* clear IPL bits → IPL=0 */
                   ::
                       : "cc");
}

static inline void arch_irq_disable(void) {
  __asm__ volatile("or.w    #0x0700,%%sr\n" /* set IPL=7 → mask all */
                   ::
                       : "cc");
}

/* ── Preemption control ──────────────────────────────────────────────────
 *
 * On single-core 68k, disabling all IRQs is sufficient and safe —
 * putc is synchronous (never blocks), so no deadlock risk.
 * ────────────────────────────────────────────────────────────────────────── */

static inline void arch_preempt_disable(void) { arch_irq_disable(); }

static inline void arch_preempt_enable(void) { arch_irq_enable(); }

/* ── Context switch trigger ───────────────────────────────────────────────
 *
 * On 68k there is no PendSV equivalent.  We set a flag that the
 * timer ISR or syscall return path checks to perform the switch.
 * For cooperative yield, TRAP #1 triggers the context switch directly.
 * ────────────────────────────────────────────────────────────────────────── */

/* Global flag: set to 1 when a context switch is needed.
 * Checked by timer ISR and syscall exit path. */
extern volatile uint32_t m68k_switch_pending;

static inline void arch_yield(void) { m68k_switch_pending = 1; }

/* ── CPU hints ────────────────────────────────────────────────────────────
 *
 * 68000 STOP instruction halts the CPU until an interrupt occurs.
 * It loads a new SR value simultaneously (must keep supervisor mode).
 * ────────────────────────────────────────────────────────────────────────── */

static inline void arch_wfi(void) {
  __asm__ volatile("stop #0x2000"); /* supervisor, IPL=0, wait for IRQ */
}

/* WFE/SEV: not applicable on single-core 68k — stub as WFI/nop */
static inline void arch_wfe(void) { arch_wfi(); }
static inline void arch_sev(void) { /* no-op on single-core */
}

/* ── Memory barriers ──────────────────────────────────────────────────────
 *
 * 68000 has a strictly-ordered memory model — no reordering.
 * These are no-ops on 68k but provided for API compatibility.
 * ────────────────────────────────────────────────────────────────────────── */

static inline void arch_dsb_isb(void) { __asm__ volatile("nop" ::: "memory"); }

static inline page_id_t arch_user_ptr_to_page(page_id_t base_page,
                                              uintptr_t user_ptr,
                                              uint16_t *off) {
  (void)base_page;
  *off = (uint16_t)((uintptr_t)user_ptr & (PAGE_SIZE - 1u));
  return mem_region_ptr_to_page((void *)(uintptr_t)user_ptr);
}

#endif /* PPAP_ARCH_M68K_ARCH_H */
