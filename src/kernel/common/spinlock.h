/*
 * spinlock.h — default (single-core) spinlock implementation
 *
 * Used by every target that does not provide an arch or target overlay
 * of this header.  On single-core targets there is no cross-core race,
 * so the lock argument is ignored and IRQ disable alone provides the
 * required exclusion against ISRs.
 *
 * Targets that need hardware spinlocks (RP2040 / RP2350 SIO block)
 * supply their own spinlock.h in arch/<arch>/kernel/common/ which
 * overrides this file via the overlay include path.  Targets that run
 * the same arch under emulation (qemu_arm, qemu_rv32) further override
 * the arch overlay with a target-level no-op overlay.
 *
 * The SPIN_* identifiers live in spinlock_ids.h so every implementation
 * shares the same enum.
 */

#ifndef PPAP_KERNEL_COMMON_SPINLOCK_H
#define PPAP_KERNEL_COMMON_SPINLOCK_H

#include <stdint.h>

#include "kernel/common/irq.h"
#include "kernel/common/spinlock_ids.h"

/*
 * Pointer to a hardware core-ID register, or NULL when the target has
 * no such register.  Used by arm_m's context-switch assembly via the
 * indirect core_id_reg pointer in proc.c.
 */
#define SPIN_CORE_ID_PTR ((volatile uint32_t *)0)

static inline uint32_t core_id(void) { return 0; }

static inline void spin_locks_reset(void) {}

static inline uint32_t spin_lock_irqsave(uint32_t lock_num) {
  (void)lock_num;
  return arch_irq_save();
}

static inline void spin_unlock_irqrestore(uint32_t lock_num, uint32_t saved) {
  (void)lock_num;
  arch_irq_restore(saved);
}

static inline void spin_lock(uint32_t lock_num) { (void)lock_num; }

static inline void spin_unlock(uint32_t lock_num) { (void)lock_num; }

/*
 * Non-blocking try-acquire.  Returns 1 if the lock was acquired (caller
 * must pair with spin_unlock), 0 if it was already held by someone else.
 * No IRQ state is touched — callers that need IRQ exclusion combine this
 * with arch_irq_save() / arch_irq_restore() themselves.
 *
 * On single-core targets there is no cross-core race, so the result is
 * always "acquired".
 */
static inline int spin_trylock(uint32_t lock_num) {
  (void)lock_num;
  return 1;
}

#endif /* PPAP_KERNEL_COMMON_SPINLOCK_H */
