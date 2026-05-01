/*
 * kstack.c — per-process kernel-stack init + overflow sentinels.
 *
 * Two implementations selected at compile time by
 * PROC_HAS_FIXED_REGION_KSTACK (config.h, set per arch + per-target
 * opt-in flags):
 *
 *   - Fixed-region: targets whose linker carves a single
 *     __kstack_region_base region into PROC_MAX consecutive slots:
 *       slot 0           : PROC_KSTACK_IDLE_SIZE bytes (idle thread)
 *       slot 1..PROC_MAX-1: PROC_KSTACK_SIZE bytes each (user procs)
 *     Each slot's last PROC_KSTACK_GUARD_BYTES bytes hold a sentinel
 *     pattern.  kernel_sp points at (true_top - GUARD_BYTES) so the
 *     active stack never overwrites it.  A second sentinel at the
 *     slot's lowest address detects this slot's own SP underrun.
 *
 *   - Empty: targets without the fixed-region mechanism.  proc_init /
 *     proc_alloc still call these so the call sites stay arch-agnostic.
 *
 * All four functions are weak so a per-arch overlay at
 * src/arch/<arch>/kernel/core/kstack.c can strong-override any single
 * function (e.g. just init_slot) without having to reimplement the
 * other three.
 */

#include "kernel/core/proc/kstack.h"

#include "kernel/common/config.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/arch.h"

#ifdef PROC_HAS_FIXED_REGION_KSTACK

/* uintptr_t-wide pattern at the slot base (2 B on ia16, 4 B on
 * 32-bit arches).  Detects this slot's own SP underrunning past its
 * base. */
#define KSTACK_CANARY_BASE ((uintptr_t)0xCA57CA57u)

/* 4-byte pattern at [true_top - 4, true_top).  Detects an adjacent
 * higher slot's SP underflowing into our guard region — invisible to
 * a base-only canary because the writes never reach our base. */
#define KSTACK_GUARD_TOP ((uint32_t)0xCAFECAFEu)

static inline uintptr_t kstack_slot_true_top(uint32_t slot_idx) {
  return (uintptr_t)__kstack_region_base + PROC_KSTACK_IDLE_SIZE +
         (uintptr_t)slot_idx * PROC_KSTACK_SIZE;
}

static inline uintptr_t kstack_slot_true_base(uint32_t slot_idx) {
  if (slot_idx == 0u) return (uintptr_t)__kstack_region_base;
  return (uintptr_t)__kstack_region_base + PROC_KSTACK_IDLE_SIZE +
         (uintptr_t)(slot_idx - 1u) * PROC_KSTACK_SIZE;
}

__attribute__((weak)) void proc_kstack_init_slot(pcb_t *p, uint32_t slot_idx) {
  p->kernel_sp = kstack_slot_true_top(slot_idx) - PROC_KSTACK_GUARD_BYTES;
}

__attribute__((weak)) void proc_kstack_plant_canary(uint32_t slot_idx) {
  uintptr_t base = kstack_slot_true_base(slot_idx);
  uintptr_t top = kstack_slot_true_top(slot_idx);
  *(volatile uintptr_t *)base = KSTACK_CANARY_BASE;
  *(volatile uint32_t *)(uintptr_t)(top - 4u) = KSTACK_GUARD_TOP;
}

__attribute__((weak)) void proc_plant_kstack_canaries(void) {
  for (uint32_t i = 0u; i < PROC_MAX; i++) proc_kstack_plant_canary(i);
}

__attribute__((weak)) void proc_check_kstack_canary_panic(void) {
  for (uint32_t i = 0u; i < PROC_MAX; i++) {
    uintptr_t base = kstack_slot_true_base(i);
    uintptr_t top = kstack_slot_true_top(i);
    uintptr_t got_base = *(volatile uintptr_t *)base;
    uint32_t got_guard = *(volatile uint32_t *)(uintptr_t)(top - 4u);
    if (got_base == KSTACK_CANARY_BASE && got_guard == KSTACK_GUARD_TOP)
      continue;
    pcb_t *cur = current;
    mod_vfs.klogf(
        "PANIC: kernel stack overrun  slot=%u base=%lx got_base=%lx"
        "  topG=%lx  pid=%u comm=%s\n",
        (unsigned)i, (unsigned long)base, (unsigned long)got_base,
        (unsigned long)got_guard, cur ? (unsigned)cur->pid : 0u,
        cur ? cur->comm : "(null)");
    for (;;) arch_wfi();
  }
}

#else

__attribute__((weak)) void proc_kstack_init_slot(pcb_t *p, uint32_t slot_idx) {
  (void)p;
  (void)slot_idx;
}

__attribute__((weak)) void proc_kstack_plant_canary(uint32_t slot_idx) {
  (void)slot_idx;
}

__attribute__((weak)) void proc_plant_kstack_canaries(void) {}

__attribute__((weak)) void proc_check_kstack_canary_panic(void) {}

#endif
