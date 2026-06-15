/*
 * arch.h — Architecture abstraction for Motorola 68000
 *
 * Provides the same API as src/arch/arm_m/arch.h but with 68k
 * implementations.  Used by shared kernel code (spinlock.h,
 * sched.c, main.c, etc.) for architecture-independent operations.
 */

#ifndef PPAP_ARCH_M68K_KERNEL_CORE_ARCH_H
#define PPAP_ARCH_M68K_KERNEL_CORE_ARCH_H

#include <stdint.h>

#include "kernel/common/irq.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/mm/region.h"

#define ARCH_EXIT_SWITCH_IN_SYSCALL_EPILOGUE 0

/* Pointer to the saved d0-d7/a0-a6/SR/PC frame at SSP, captured by trap.S
 * at TRAP #0 entry.  signal_check / sys_rt_sigreturn read/overwrite the
 * SR/PC slots so the trap-exit RTE lands in the desired user context.
 * Zero outside a syscall trap.  Defined in m68k_common.c. */
extern volatile uint32_t m68k_trap_frame_sp;

/* ── Context switch trigger ───────────────────────────────────────────────
 *
 * On 68k there is no PendSV equivalent.  We set a flag that the
 * timer ISR or syscall return path checks to perform the switch.
 * For cooperative yield, TRAP #1 triggers the context switch directly.
 * ────────────────────────────────────────────────────────────────────────── */

/* Immediate thread-context switch via TRAP #1 — m68k_trap1_handler does
 * the context switch immediately.  arch_yield() only sets a flag, which
 * is insufficient from thread context where no timer ISR is pending to
 * check it. */
#define ARCH_HAS_SCHED_SWITCH
static inline void arch_sched_switch(void) { __asm__ volatile("trap #1"); }

/* Shared flag-based yield: switch_pending is set to 1 by arch_yield() and
 * checked by timer ISR / trap-return path (per-arch assembly). */
#include "kernel/common/arch_yield_default.h"

/* ── Scheduler startup hook ────────────────────────────────────────────────
 *
 * Called from the shared sched_start() before arch_irq_enable().
 * m68k has no PSP/MSP split and no priority registers to tweak — timer
 * ISR setup is done by target_late_init() — so the hook is a no-op. */
static inline void arch_sched_start_hook(void) {}

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
static inline void arch_sev(void) { /* no-op on single-core */ }

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
  return page_from_ptr((void *)(uintptr_t)user_ptr);
}

/* ── vfork parent-resume hooks ─────────────────────────────────────────────
 *
 * Save the 4-byte user-stack slot at the parent's USP (the bsr-pushed
 * return address of the vfork() caller) before the child runs.  The child
 * uses the same user stack until execve()/_exit(), so its execve argument
 * setup can overwrite that slot.  See
 * docs/proposals/no_stack_copy_on_vfork.md.
 * ────────────────────────────────────────────────────────────────────────── */

struct pcb;
void m68k_vfork_save_parent_frame(struct pcb *parent);
void m68k_vfork_restore_frame(void);

#endif /* PPAP_ARCH_M68K_KERNEL_CORE_ARCH_H */
