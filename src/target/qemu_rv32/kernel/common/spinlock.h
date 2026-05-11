/*
 * spinlock.h — no-op overlay for qemu_rv32
 *
 * The virt machine has no SIO block, so we replace the riscv arch
 * overlay (which assumes RP2350 SIO) with the same no-op single-core
 * implementation that the primary kernel/common/spinlock.h uses on
 * m68k, xtensa and i16.
 */

#ifndef PPAP_TARGET_QEMU_RV32_KERNEL_COMMON_SPINLOCK_H
#define PPAP_TARGET_QEMU_RV32_KERNEL_COMMON_SPINLOCK_H

#include <stdint.h>

#include "kernel/common/irq.h"
#include "kernel/common/spinlock_ids.h"

/* virt machine has no SIO block; the core_id_reg mechanism is unused
 * on RISC-V today, but define the macro for parity. */
#define SPIN_CORE_ID_PTR ((volatile uint32_t *)0)

static inline uint32_t core_id(void) {
  return 0;
}

static inline void spin_locks_reset(void) {
}

static inline uint32_t spin_lock_irqsave(uint32_t lock_num) {
  (void)lock_num;
  return arch_irq_save();
}

static inline void spin_unlock_irqrestore(uint32_t lock_num, uint32_t saved) {
  (void)lock_num;
  arch_irq_restore(saved);
}

static inline void spin_lock(uint32_t lock_num) {
  (void)lock_num;
}

static inline void spin_unlock(uint32_t lock_num) {
  (void)lock_num;
}

static inline int spin_trylock(uint32_t lock_num) {
  (void)lock_num;
  return 1;
}

#endif /* PPAP_TARGET_QEMU_RV32_KERNEL_COMMON_SPINLOCK_H */
