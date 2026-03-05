/*
 * spinlock.h — RP2040 hardware spinlock API
 *
 * The RP2040 SIO block provides 32 hardware spinlocks at SIO_BASE+0x100.
 * Each lock is a single 32-bit register:
 *   - Read: try-acquire.  Returns non-zero on success, 0 if already held.
 *   - Write (any value): release.
 *
 * Pattern: disable local IRQs before acquire, re-enable after release.
 * This prevents deadlock if an ISR tries to acquire the same lock.
 *
 * On QEMU (mps2-an500) the SIO block does not exist — reads from
 * 0xD0000000+ would return 0 or fault.  We detect QEMU via SCB.CPUID
 * (Cortex-M0+ PARTNO = 0xC60) and skip the hardware lock, relying on
 * IRQ disable alone (sufficient for single-core).
 */

#ifndef PPAP_SPINLOCK_H
#define PPAP_SPINLOCK_H

#include <stdint.h>

#define SIO_BASE            0xD0000000u
#define SIO_CPUID           (*(volatile uint32_t *)(SIO_BASE + 0x000u))
#define SIO_SPINLOCK_BASE   (SIO_BASE + 0x100u)

/* SCB.CPUID — always accessible on any Cortex-M */
#define SCB_CPUID_REG       (*(volatile uint32_t *)0xE000ED00u)
#define CPUID_PARTNO_MASK   0x0000FFF0u
#define CPUID_PARTNO_M0P    0x0000C600u

enum {
    SPIN_PAGE = 0,   /* free_stack, free_top (page allocator) */
    SPIN_PROC = 1,   /* proc_table, next_pid, running_on_core */
    SPIN_VFS  = 2,   /* mount table, vnode pool */
    SPIN_FS   = 3,   /* sector_buf (vfat.c), ufs_buf (ufs.c) */
    SPIN_UART = 4,   /* UART TX serialisation (klog) */
};

static inline int spin_have_hw(void)
{
    return (SCB_CPUID_REG & CPUID_PARTNO_MASK) == CPUID_PARTNO_M0P;
}

static inline uint32_t core_id(void)
{
#ifdef PPAP_QEMU
    return 0;   /* QEMU: single core, no SIO */
#else
    return SIO_CPUID;   /* RP2040: single MMIO read, ~1 cycle */
#endif
}

/*
 * Release all 32 hardware spinlocks.
 *
 * Must be called once at early boot before any spin_lock_irqsave().
 * On RP2040, the SIO block is NOT reset by a Core 0 reset (e.g. GDB
 * reload + `monitor reset halt`).  If the previous session was
 * interrupted while a spinlock was held, the lock stays claimed and
 * the first acquire in the new session hangs forever.
 *
 * The pico-sdk does the same in runtime_init → spin_locks_reset().
 */
static inline void spin_locks_reset(void)
{
    if (!spin_have_hw())
        return;
    for (uint32_t i = 0; i < 32u; i++) {
        volatile uint32_t *lock =
            (volatile uint32_t *)(SIO_SPINLOCK_BASE + i * 4u);
        *lock = 0u;   /* write any value to release */
    }
}

static inline uint32_t spin_lock_irqsave(uint32_t lock_num)
{
    uint32_t saved;
    __asm__ volatile ("mrs %0, primask" : "=r"(saved));
    __asm__ volatile ("cpsid i");
    if (spin_have_hw()) {
        volatile uint32_t *lock =
            (volatile uint32_t *)(SIO_SPINLOCK_BASE + lock_num * 4u);
        while (!*lock)
            ;
    }
    return saved;
}

static inline void spin_unlock_irqrestore(uint32_t lock_num, uint32_t saved)
{
    if (spin_have_hw()) {
        volatile uint32_t *lock =
            (volatile uint32_t *)(SIO_SPINLOCK_BASE + lock_num * 4u);
        *lock = 0u;
    }
    __asm__ volatile ("msr primask, %0" :: "r"(saved));
}

#endif /* PPAP_SPINLOCK_H */
