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
#include "kernel/common/ioregs.h" /* SCB_ICSR, PENDSVSET */
#include "kernel/common/irq.h"
#include "kernel/core/mm/mem_region.h"

/* ARM-M vfork still manufactures the child frame on a copied process stack. */
#define ARCH_VFORK_COPY_PROCESS_STACK 1
#define ARCH_VFORK_CHILD_FRAME_POINTER 1
#define ARCH_EXIT_SWITCH_IN_SYSCALL_EPILOGUE 0

extern volatile uint32_t arm_exc_return[2];
extern volatile uint32_t svc_saved_msp[2];

/* User-side HW exception frame builder.  Pairs with arch_build_initial_frame
 * which writes the kernel-side SW frame on the kernel slot. */
uint32_t *arm_build_user_hw_frame(uint32_t *sp, void (*entry)(void));

/* ── Context switch trigger ─────────────────────────────────────────────────
 */

/* Pend PendSV exception for async preemption or restart-style blocking.
 * ARM Cortex-M does not use the shared switch_pending flag because PendSV
 * self-pends via NVIC and runs as soon as exceptions return.  Define
 * ARCH_HAS_YIELD before the include so the default is skipped. */
#define ARCH_HAS_YIELD
static inline void arch_yield(void) { SCB_ICSR |= PENDSVSET; }

#define ARCH_HAS_SCHED_SWITCH
void arm_kernel_sched_switch(void);
int arm_can_kernel_sched_switch(void);
/* sched_switch() has two ARM meanings:
 * - a blocked non-restart SVC continuation switches immediately by saving the
 *   live MSP call chain in arm_kernel_sched_switch();
 * - async preemption and restart-style blocking pend PendSV and switch after
 *   exception return.
 */
static inline void arch_sched_switch(void) {
  uint32_t ipsr;
  __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
  if (ipsr != 0u && arm_can_kernel_sched_switch()) {
    arm_kernel_sched_switch();
  } else {
    arch_yield();
  }
}

#define ARCH_HAS_YIELD_CONSUME
static inline int arch_yield_consume(void) { return 0; }

#include "kernel/common/arch_yield_default.h"

/* ── Scheduler startup hook ─────────────────────────────────────────────────
 *
 * Called from the shared sched_start() before arch_irq_enable().  Must be
 * static inline: sched_start() switches Thread mode from MSP to PSP inside
 * this body, and any real function call would force the compiler to emit
 * a push/pop of LR that would straddle the stack switch (push on MSP, pop
 * on PSP) and corrupt the return path.  The inline arithmetic on
 * stack_page_id is deliberate for the same reason — mem_region_page_linear()
 * is an extern call and cannot be used here.
 * ────────────────────────────────────────────────────────────────────────── */
static inline void arch_sched_start_hook(void) {
  /* Set PendSV to lowest priority (0xFF) so it never preempts real IRQs.
   * SHPR3[23:16] is the PendSV priority byte on Cortex-M0+. */
  SCB_SHPR3 = (SCB_SHPR3 & ~PENDSV_PRIO_MASK) | PENDSV_PRIO_LOWEST;

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
  return mem_region_ptr_to_page((void *)(uintptr_t)user_ptr);
}

#endif /* PPAP_ARCH_ARM_M_KERNEL_CORE_ARCH_H */
