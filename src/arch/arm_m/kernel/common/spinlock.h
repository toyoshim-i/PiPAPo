/*
 * spinlock.h — RP2040 / RP2350 hardware spinlock implementation
 *
 * The SIO block exposes 32 hardware spinlocks at SIO_BASE+0x100.
 * Each lock is a single 32-bit register:
 *   - Read: try-acquire.  Returns non-zero on success, 0 if already held.
 *   - Write (any value): release.
 *
 * Pattern: disable local IRQs before acquire, re-enable after release.
 * This prevents deadlock if an ISR tries to acquire the same lock.
 *
 * This file overrides kernel/common/spinlock.h on every arm_m target
 * that does NOT supply its own target-level overlay.  qemu_arm supplies
 * a no-op overlay since the mps2-an500 machine has no SIO block.
 */

#ifndef PPAP_ARCH_ARM_M_KERNEL_COMMON_SPINLOCK_H
#define PPAP_ARCH_ARM_M_KERNEL_COMMON_SPINLOCK_H

#include <stdint.h>

#include "kernel/common/irq.h"
#include "kernel/common/spinlock_ids.h"

#define SIO_BASE 0xD0000000u
#define SIO_CPUID (*(volatile uint32_t *)(SIO_BASE + 0x000u))
#define SIO_SPINLOCK_BASE (SIO_BASE + 0x100u)

/* SIO_CPUID lives at SIO_BASE+0; arm_m switch.S indirects through
 * core_id_reg to read it cheaply from assembly. */
#define SPIN_CORE_ID_PTR ((volatile uint32_t *)SIO_BASE)

static inline uint32_t core_id(void) { return SIO_CPUID; }

/*
 * Release all 32 hardware spinlocks.
 *
 * Must be called once at early boot before any spin_lock_irqsave().
 * The SIO block is NOT reset by a Core 0 reset (e.g. GDB reload +
 * `monitor reset halt`).  If the previous session was interrupted
 * while a spinlock was held, the lock stays claimed and the first
 * acquire in the new session hangs forever.
 *
 * The pico-sdk does the same in runtime_init -> spin_locks_reset().
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

#endif /* PPAP_ARCH_ARM_M_KERNEL_COMMON_SPINLOCK_H */
