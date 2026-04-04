/*
 * arch.h — Architecture abstraction for Xtensa LX7 (ESP32-S3)
 *
 * Provides the same API as src/arch/arm_m/arch.h, src/arch/m68k/arch.h,
 * and src/arch/riscv/arch.h but with Xtensa implementations.
 *
 * Windowed ABI: ESP-IDF requires windowed register ABI.
 * Context switch uses solicited-frame pattern with window spill.
 */

#ifndef PPAP_ARCH_XTENSA_ARCH_H
#define PPAP_ARCH_XTENSA_ARCH_H

#include <stdint.h>
#include "kernel/core/mm/mem_region.h"
#include "cpu.h"

/* ── Interrupt save / restore ─────────────────────────────────────────────
 *
 * Xtensa uses PS.INTLEVEL (bits 3:0) to mask interrupts.  INTLEVEL=0
 * enables all; INTLEVEL=15 disables all.  The RSIL instruction atomically
 * reads the old PS and sets INTLEVEL in one step.
 * ────────────────────────────────────────────────────────────────────────── */

static inline uint32_t arch_irq_save(void)
{
    uint32_t old_ps;
    __asm__ volatile ("rsil %0, 15" : "=a"(old_ps));  /* INTLEVEL=15 */
    return old_ps;
}

static inline void arch_irq_restore(uint32_t saved)
{
    __asm__ volatile ("wsr %0, ps\n\t"
                      "rsync"
                      :: "a"(saved));
}

/* ── Interrupt enable / disable ───────────────────────────────────────── */

static inline void arch_irq_enable(void)
{
    uint32_t ps;
    __asm__ volatile ("rsr %0, ps" : "=a"(ps));
    ps &= ~PS_INTLEVEL_MASK;  /* INTLEVEL=0: all interrupts enabled */
    __asm__ volatile ("wsr %0, ps\n\t"
                      "rsync"
                      :: "a"(ps));
}

static inline void arch_irq_disable(void)
{
    uint32_t dummy;
    __asm__ volatile ("rsil %0, 15" : "=a"(dummy));  /* INTLEVEL=15 */
}

/* ── Preemption control ──────────────────────────────────────────────────
 *
 * Toggle the timer interrupt (CCOMPARE0) in the INTENABLE register so
 * that only the preemption timer is affected, leaving other interrupts
 * active.  Matches the ARM arch_preempt_disable/enable semantics.
 *
 * xtensa_timer_ready is set by xtensa_timer_init().  Before that,
 * arch_preempt_enable() is a no-op to avoid enabling a stale CCOMPARE0
 * interrupt that has no registered handler yet.
 * ────────────────────────────────────────────────────────────────────────── */

extern volatile uint32_t xtensa_timer_ready;

static inline void arch_preempt_disable(void)
{
    uint32_t intenable;
    __asm__ volatile ("rsr %0, intenable" : "=a"(intenable));
    intenable &= ~XTENSA_TIMER0_INTMASK;
    __asm__ volatile ("wsr %0, intenable\n\t"
                      "rsync"
                      :: "a"(intenable));
}

static inline void arch_preempt_enable(void)
{
    if (!xtensa_timer_ready) return;  /* timer not initialized yet */
    uint32_t intenable;
    __asm__ volatile ("rsr %0, intenable" : "=a"(intenable));
    intenable |= XTENSA_TIMER0_INTMASK;
    __asm__ volatile ("wsr %0, intenable\n\t"
                      "rsync"
                      :: "a"(intenable));
}

/* ── Context switch trigger ──────────────────────────────────────────────
 *
 * Xtensa has no PendSV equivalent.  We use the RISC-V/m68k pattern:
 * set a flag that the timer ISR checks after calling sched_timer_tick().
 * ────────────────────────────────────────────────────────────────────────── */

extern volatile uint32_t xtensa_switch_pending;

static inline void arch_yield(void)
{
    xtensa_switch_pending = 1;
}

/* ── CPU hints ────────────────────────────────────────────────────────── */

static inline void arch_wfi(void)
{
    __asm__ volatile ("waiti 0");
}

/* WFE: not a standard Xtensa instruction — use WAITI as fallback */
static inline void arch_wfe(void) { arch_wfi(); }

/* SEV: on ESP32-S3, inter-core wakeup uses crosscore interrupt.
 * For now, stub as no-op (single-core initial port). */
static inline void arch_sev(void) { /* no-op on single-core */ }

/* ── Memory barriers ──────────────────────────────────────────────────── */

static inline void arch_dsb_isb(void)
{
    __asm__ volatile ("memw" ::: "memory");   /* data memory barrier */
    __asm__ volatile ("isync" ::: "memory");  /* instruction sync */
}

static inline page_id_t arch_user_ptr_to_page(page_id_t base_page,
                                              uintptr_t user_ptr,
                                              uint16_t *off)
{
    (void)base_page;
    *off = (uint16_t)((uintptr_t)user_ptr & (PAGE_SIZE - 1u));
    return mem_region_ptr_to_page((void *)(uintptr_t)user_ptr);
}

#endif /* PPAP_ARCH_XTENSA_ARCH_H */
