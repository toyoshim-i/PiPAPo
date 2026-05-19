/*
 * arch.h — Architecture abstraction for ARM Cortex-M (M-profile)
 *
 * Provides inline functions for architecture-specific operations used by
 * the kernel: interrupt save/restore, yield, barriers, etc.
 *
 * Future architectures (m68k, arm_a) will provide their own arch.h with
 * the same API but different implementations.
 */

#ifndef PPAP_ARCH_ARM_M_KERNEL_CORE_ARCH_H
#define PPAP_ARCH_ARM_M_KERNEL_CORE_ARCH_H

#include <stdint.h>

#include "kernel/common/core/proc_info.h"
#include "kernel/common/ioregs.h"
#include "kernel/common/irq.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/mm/region.h"

#define ARCH_EXIT_SWITCH_IN_SYSCALL_EPILOGUE 0

extern volatile uint32_t arm_exc_return[2];

/* User-side HW exception frame builder.  Pairs with arch_build_initial_frame
 * which writes the kernel-side SW frame on the kernel slot. */
uint32_t *arm_build_user_hw_frame(uint32_t *sp, void (*entry)(void));

/* ── vfork parent-resume hooks ─────────────────────────────────────────────
 *
 * No-copy vfork on arm: save the parent's 32-byte HW exception frame plus
 * the vfork stub's 8-byte {r7, lr} push (40 B total) from the live user PSP
 * into the parent's PCB.  After the save, overwrite live user_PSP+0 with 0
 * so the child's HW frame pop yields r0 = 0.  arm_vfork_restore_frame()
 * writes the saved 40 B back before the parent's next user-mode bx EXC_RETURN
 * — Slot 0 of the saved frame already holds the patched r0 = child_pid
 * (from Step 7's frame[0] = child->pid in sys_vfork).
 * ────────────────────────────────────────────────────────────────────────── */

struct pcb;
void arm_vfork_save_parent_frame(struct pcb *parent, uint32_t *user_psp);
void arm_vfork_restore_frame(void);

/* ── Context switch trigger ─────────────────────────────────────────────────
 *
 * ARM Cortex-M now uses the shared switch_pending flag (PendSV retired).
 * SysTick exit checks the flag and calls arm_kernel_sched_switch when set.
 * Thread-mode arch_sched_switch sets the flag too — the next IRQ exit
 * (typically SysTick within one tick) honors it.
 * ────────────────────────────────────────────────────────────────────────── */

#define ARCH_HAS_SCHED_SWITCH
void arm_kernel_sched_switch(void);

/* Pull in the shared switch_pending-based arch_yield first, so the
 * arch_sched_switch inline below can call it. */
#include "kernel/common/arch_yield_default.h"

/* sched_switch() on ARM:
 * - From Handler mode (in-syscall sched_switch): arm_kernel_sched_switch
 *   performs the unified MSP+PSP swap immediately.
 * - From Thread mode (idle loop, kernel-resident subsystems like cpm
 *   reaching sys_exit, etc.): issue a sentinel SVC.  The SVC handler
 *   detects imm=0xFF and calls arm_kernel_sched_switch directly without
 *   running the syscall dispatch.  Same shape m68k uses with trap #1. */
static inline void arch_sched_switch(void) {
  uint32_t ipsr;
  __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
  if (ipsr != 0u) {
    arm_kernel_sched_switch();
  } else {
    __asm__ volatile("svc #0xFF" ::: "memory");
  }
}

/* ── Scheduler startup hook ─────────────────────────────────────────────────
 *
 * Called from the shared sched_start() before arch_irq_enable().  Must be
 * static inline: sched_start() switches Thread mode from MSP to PSP inside
 * this body, and any real function call would force the compiler to emit
 * a push/pop of LR that would straddle the stack switch (push on MSP, pop
 * on PSP) and corrupt the return path.  The inline arithmetic on
 * stack_page_id is deliberate for the same reason — page_linear()
 * is an extern call and cannot be used here.
 * ────────────────────────────────────────────────────────────────────────── */
static inline void arch_sched_start_hook(void) {
  /* Lower SVCall priority (0x80) so hardware interrupts (SysTick, UART)
   * can preempt the SVC handler.  Without this, WFI inside blocking
   * syscalls (e.g. tty_read) would never wake — no interrupt can preempt
   * a handler at the default priority 0x00. */
  SCB_SHPR2 = (SCB_SHPR2 & ~SVCALL_PRIO_MASK) | (0x80u << SVCALL_PRIO_SHIFT);

  /* Switch Thread mode from MSP to PSP using Thread 0's dedicated stack. */
  uint32_t psp_top =
      (uint32_t)(proc_table[0].stack_page_id * PAGE_SIZE) + PAGE_SIZE;
  __asm__ volatile(
      "msr  psp, %0      \n" /* PSP = top of Thread 0's stack page */
      "movs r0, #2       \n" /* CONTROL.SPSEL = 1 */
      "msr  control, r0  \n"
      "isb               \n" ::"r"(psp_top)
      : "r0");

  /* Configure SysTick: reload value, clear current count, start.
   * SYSTICK_RELOAD is duplicated here rather than including sched.h, which
   * would create an arch.h ↔ proc.h ↔ sched.h include cycle. */
  SYST_RVR = (PPAP_SYS_HZ / PPAP_TICK_HZ - 1u);
  SYST_CVR = 0u;
  SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;
}

/* ── CPU hints ──────────────────────────────────────────────────────────────
 */

/* Wait for interrupt — puts the CPU in low-power sleep until an IRQ fires. */
static inline void arch_wfi(void) { __asm__ volatile("wfi"); }

/* Wait for event — used for inter-core synchronisation on RP2040. */
static inline void arch_wfe(void) { __asm__ volatile("wfe"); }

/* Send event — wakes a core sleeping in WFE. */
static inline void arch_sev(void) { __asm__ volatile("sev"); }

/* ── Memory barriers ────────────────────────────────────────────────────────
 */

/* Data synchronisation barrier + instruction synchronisation barrier.
 * Used after MPU reprogramming to ensure new settings take effect. */
static inline void arch_dsb_isb(void) {
  __asm__ volatile("dsb\n isb" ::: "memory");
}

static inline page_id_t arch_user_ptr_to_page(page_id_t base_page,
                                              uintptr_t user_ptr,
                                              uint16_t *off) {
  (void)base_page;
  *off = (uint16_t)((uintptr_t)user_ptr & (PAGE_SIZE - 1u));
  return page_from_ptr((void *)(uintptr_t)user_ptr);
}

#endif /* PPAP_ARCH_ARM_M_KERNEL_CORE_ARCH_H */
