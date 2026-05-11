/*
 * spinlock.h — RP2350 hardware spinlock implementation (RISC-V cores)
 *
 * The Hazard3 RISC-V cores share the same SIO block as the M33 cores,
 * so spinlocks at SIO_BASE+0x100 work identically.  See the arm_m
 * overlay for the SIO protocol details.
 *
 * This file overrides kernel/common/spinlock.h on every riscv target
 * that does NOT supply its own target-level overlay.  qemu_rv32
 * supplies a no-op overlay since the virt machine has no SIO block.
 */

#ifndef PPAP_ARCH_RISCV_KERNEL_COMMON_SPINLOCK_H
#define PPAP_ARCH_RISCV_KERNEL_COMMON_SPINLOCK_H

#include <stdint.h>

#include "kernel/common/irq.h"
#include "kernel/common/spinlock_ids.h"

#define SIO_BASE 0xD0000000u
#define SIO_CPUID (*(volatile uint32_t *)(SIO_BASE + 0x000u))
#define SIO_SPINLOCK_BASE (SIO_BASE + 0x100u)

/* SIO_CPUID lives at SIO_BASE+0.  Not currently used from RISC-V
 * assembly, but provided for parity with the arm_m overlay. */
#define SPIN_CORE_ID_PTR ((volatile uint32_t *)SIO_BASE)

static inline uint32_t core_id(void) { return SIO_CPUID; }

/*
 * Release all 32 hardware spinlocks.  See arm_m overlay for rationale.
 */
static inline void spin_locks_reset(void) {
  for (uint32_t i = 0; i < 32u; i++) {
    volatile uint32_t *lock = (volatile uint32_t *)(SIO_SPINLOCK_BASE + i * 4u);
    *lock = 0u;
  }
}

static inline uint32_t spin_lock_irqsave(uint32_t lock_num) {
  uint32_t saved = arch_irq_save();
  volatile uint32_t *lock =
      (volatile uint32_t *)(SIO_SPINLOCK_BASE + lock_num * 4u);
  while (!*lock);
  return saved;
}

static inline void spin_unlock_irqrestore(uint32_t lock_num, uint32_t saved) {
  volatile uint32_t *lock =
      (volatile uint32_t *)(SIO_SPINLOCK_BASE + lock_num * 4u);
  *lock = 0u;
  arch_irq_restore(saved);
}

static inline void spin_lock(uint32_t lock_num) {
  volatile uint32_t *lock =
      (volatile uint32_t *)(SIO_SPINLOCK_BASE + lock_num * 4u);
  while (!*lock);
}

static inline void spin_unlock(uint32_t lock_num) {
  volatile uint32_t *lock =
      (volatile uint32_t *)(SIO_SPINLOCK_BASE + lock_num * 4u);
  *lock = 0u;
}

/*
 * Non-blocking try-acquire.  Returns 1 if the lock was acquired (caller
 * must pair with spin_unlock), 0 if it was already held by someone else.
 * No IRQ state is touched — callers that need IRQ exclusion combine this
 * with arch_irq_save() / arch_irq_restore() themselves.
 */
static inline int spin_trylock(uint32_t lock_num) {
  volatile uint32_t *lock =
      (volatile uint32_t *)(SIO_SPINLOCK_BASE + lock_num * 4u);
  /* SIO read returns the lock number on acquire, 0 if already held. */
  return *lock != 0u;
}

#endif /* PPAP_ARCH_RISCV_KERNEL_COMMON_SPINLOCK_H */
