/*
 * arch.h — Architecture abstraction for RISC-V (RV32IMAC)
 *
 * Provides the same API as src/arch/arm_m/arch.h and src/arch/m68k/arch.h
 * but with RISC-V implementations.  Used by shared kernel code (spinlock.h,
 * sched.c, main.c, etc.) for architecture-independent operations.
 */

#ifndef PPAP_ARCH_RISCV_ARCH_H
#define PPAP_ARCH_RISCV_ARCH_H

#include <stdint.h>
#include "../../kernel/mm/mem_region.h"
#include "cpu.h"

/* ── Interrupt save / restore ─────────────────────────────────────────────
 *
 * RISC-V uses mstatus.MIE (bit 3) to globally enable/disable M-mode
 * interrupts.  We save the MIE bit and clear it atomically with csrrc.
 * ────────────────────────────────────────────────────────────────────────── */

static inline uint32_t arch_irq_save(void)
{
    uint32_t mstatus;
    __asm__ volatile ("csrrc %0, mstatus, %1"
                      : "=r"(mstatus)
                      : "r"(MSTATUS_MIE));
    return mstatus & MSTATUS_MIE;
}

static inline void arch_irq_restore(uint32_t saved)
{
    if (saved)
        __asm__ volatile ("csrs mstatus, %0" :: "r"(MSTATUS_MIE));
}

/* ── Interrupt enable / disable ───────────────────────────────────────── */

static inline void arch_irq_enable(void)
{
    __asm__ volatile ("csrs mstatus, %0" :: "r"(MSTATUS_MIE));
}

static inline void arch_irq_disable(void)
{
    __asm__ volatile ("csrc mstatus, %0" :: "r"(MSTATUS_MIE));
}

/* ── Preemption control ──────────────────────────────────────────────────
 *
 * Toggle the Machine Timer Interrupt Enable (mie.MTIE) so that only
 * the preemption timer is affected, leaving other interrupts active.
 * Matches the ARM arch_preempt_disable/enable semantics (SysTick TICKINT).
 * ────────────────────────────────────────────────────────────────────────── */

static inline void arch_preempt_disable(void)
{
    __asm__ volatile ("csrc mie, %0" :: "r"(MIE_MTIE));
}

static inline void arch_preempt_enable(void)
{
    __asm__ volatile ("csrs mie, %0" :: "r"(MIE_MTIE));
}

/* ── Context switch trigger ──────────────────────────────────────────────
 *
 * RISC-V has no PendSV equivalent.  We use the m68k pattern: set a flag
 * that the timer ISR checks after calling sched_timer_tick().
 * For cooperative yield, ecall from M-mode triggers the switch directly.
 * ────────────────────────────────────────────────────────────────────────── */

extern volatile uint32_t riscv_switch_pending;

static inline void arch_yield(void)
{
    riscv_switch_pending = 1;
}

/* ── CPU hints ────────────────────────────────────────────────────────── */

static inline void arch_wfi(void)
{
    __asm__ volatile ("wfi");
}

/* WFE: not a standard RISC-V instruction — use WFI as fallback */
static inline void arch_wfe(void) { arch_wfi(); }

/* SEV: on RP2350 RISC-V, inter-core wakeup uses SIO doorbell.
 * For now, stub as no-op (single-core initial port). */
static inline void arch_sev(void) { /* no-op on single-core */ }

/* ── Memory barriers ──────────────────────────────────────────────────── */

static inline void arch_dsb_isb(void)
{
    __asm__ volatile ("fence rw, rw" ::: "memory");
    __asm__ volatile ("fence.i" ::: "memory");
}

/* ── User-space byte read ──────────────────────────────────────────────────
 * RISC-V SRAM supports byte access — plain dereference is fine. */
static inline uint8_t read_user_byte(const uint8_t *ptr) { return *ptr; }

static inline page_id_t arch_user_ptr_to_page(page_id_t base_page,
                                              uintptr_t user_ptr,
                                              uint16_t *off) {
  (void)base_page;
  *off = (uint16_t)((uintptr_t)user_ptr & (PAGE_SIZE - 1u));
  return mem_region_ptr_to_page((void *)(uintptr_t)user_ptr);
}

#endif /* PPAP_ARCH_RISCV_ARCH_H */
