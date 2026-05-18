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

#define ARCH_EXIT_SWITCH_IN_SYSCALL_EPILOGUE 0

/* Pointer to the saved trap frame at the base of the current kernel trap
 * stack frame, captured by trap.S in the ecall (syscall) path.
 * signal_check / sys_rt_sigreturn read/overwrite the mepc/mstatus/sp slots
 * so the trap-exit mret lands in the desired user context.  Zero outside
 * a syscall trap.  Defined in riscv_common.c. */
extern volatile uint32_t rv32_trap_frame_sp;

/* ── Context switch trigger ──────────────────────────────────────────────
 *
 * RISC-V has no PendSV equivalent.  arch_yield() uses the shared
 * switch_pending flag for interrupt-return preemption, while
 * sched_switch() executes a machine-mode ecall so the trap frame captures
 * the live kernel call chain and resumes it when the process runs again.
 * ────────────────────────────────────────────────────────────────────────── */

#define ARCH_HAS_SCHED_SWITCH
static inline void arch_sched_switch(void) {
  __asm__ volatile("ecall" ::: "memory");
}

/* Shared flag-based yield.  See kernel/common/arch_yield_default.h. */
#include "kernel/common/arch_yield_default.h"

/* ── Scheduler startup hook ────────────────────────────────────────────────
 *
 * Called from the shared sched_start() before arch_irq_enable().
 * Sets mscratch to pid 0's fixed kernel-stack slot.  boot.S initialized it to
 * __stack_top (linker stack), but scheduler entry runs with per-process
 * kernel stacks.
 * Must be done before enabling interrupts. */
#include "kernel/common/core/proc_info.h" /* proc_table */
static inline void arch_sched_start_hook(void) {
  __asm__ volatile("csrw mscratch, %0" : : "r"(proc_table[0].kernel_sp));
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
  return page_from_ptr((void *)(uintptr_t)user_ptr);
}

#endif /* PPAP_ARCH_RISCV_KERNEL_CORE_ARCH_H */
