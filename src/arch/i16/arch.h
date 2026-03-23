/*
 * arch.h -- Architecture abstraction for i8086 (16-bit real mode)
 *
 * Provides the same API as src/arch/arm_m/arch.h, src/arch/m68k/arch.h,
 * and src/arch/riscv/arch.h but with i8086 real-mode implementations.
 *
 * Phase P-1: Stubs only -- just enough to compile the boot banner.
 * Phase P-2 will add real timer/preemption/context-switch support.
 */

#ifndef PPAP_ARCH_I16_ARCH_H
#define PPAP_ARCH_I16_ARCH_H

#include <stdint.h>

/* -- Interrupt save / restore ----------------------------------------------
 *
 * i8086 uses the FLAGS register IF bit (bit 9) to enable/disable
 * maskable interrupts.  CLI clears IF, STI sets it.
 * pushf/popf save/restore the full FLAGS register.
 * ---------------------------------------------------------------------- */

static inline uint16_t arch_irq_save(void)
{
  uint16_t flags;
  __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags));
  return flags & 0x0200u;  /* Isolate IF bit */
}

static inline void arch_irq_restore(uint16_t saved)
{
  if (saved)
    __asm__ volatile ("sti");
}

/* -- Interrupt enable / disable ----------------------------------------- */

static inline void arch_irq_enable(void)
{
  __asm__ volatile ("sti");
}

static inline void arch_irq_disable(void)
{
  __asm__ volatile ("cli");
}

/* -- Preemption control -------------------------------------------------
 *
 * TODO: implement PIT IRQ0 mask via 8259A OCW1 for fine-grained
 * preemption control (P-2).  For now, alias to global IRQ toggle.
 * ------------------------------------------------------------------- */

static inline void arch_preempt_disable(void)
{
  arch_irq_disable();
}

static inline void arch_preempt_enable(void)
{
  arch_irq_enable();
}

/* -- Context switch trigger ---------------------------------------------
 *
 * Flag-driven context switch, same pattern as m68k and RISC-V.
 * The timer ISR checks this flag and performs the switch if set.
 * ------------------------------------------------------------------- */

extern volatile uint16_t i16_switch_pending;

static inline void arch_yield(void)
{
  i16_switch_pending = 1;
}

/* -- CPU hints ---------------------------------------------------------- */

static inline void arch_wfi(void)
{
  __asm__ volatile ("hlt");
}

static inline void arch_wfe(void) { arch_wfi(); }
static inline void arch_sev(void) { /* no-op, single core */ }

/* -- Memory barriers ----------------------------------------------------
 *
 * 8086 is single-core with in-order execution.  No barriers needed.
 * ------------------------------------------------------------------- */

static inline void arch_dsb_isb(void)
{
  __asm__ volatile ("" ::: "memory");  /* Compiler barrier only */
}

/* -- User-space byte read -----------------------------------------------
 *
 * Real mode: all memory is directly accessible.  Plain dereference.
 * ------------------------------------------------------------------- */

static inline uint8_t read_user_byte(const uint8_t *ptr)
{
  return *ptr;
}

#endif /* PPAP_ARCH_I16_ARCH_H */
