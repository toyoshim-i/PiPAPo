/*
 * kstack.c — per-process kernel-stack init + overflow sentinels.
 *
 * PROC_HAS_FIXED_REGION_KSTACK selects targets whose linker carves a single
 * __kstack_region_base region into PROC_MAX consecutive slots:
 *
 *   slot 0             : PROC_KSTACK_IDLE_SIZE bytes (idle thread)
 *   slot 1..PROC_MAX-1 : PROC_KSTACK_SIZE bytes each (user procs)
 *
 * Each slot reserves a uintptr_t base canary and a PROC_KSTACK_GUARD_BYTES
 * top guard.  kernel_sp points at the payload top so normal stack growth does
 * not overwrite the guard.
 *
 * Targets without the fixed region use empty helpers.  The public functions
 * stay available so proc_init/proc_alloc call sites remain arch-agnostic.
 *
 * Public functions are weak so a per-arch overlay at
 * src/arch/<arch>/kernel/core/kstack.c can strong-override any single
 * function without reimplementing the rest.
 */

#include "kernel/core/proc/kstack.h"

#include "kernel/common/config.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/arch.h"

/* ── Private implementation helpers ─────────────────────────────────────── */

#ifdef PROC_HAS_FIXED_REGION_KSTACK

/* uintptr_t-wide pattern at the slot base (2 B on ia16, 4 B on
 * 32-bit arches).  Detects this slot's own SP underrunning past its base. */
#define KSTACK_CANARY_BASE ((uintptr_t)0xCA57CA57u)

/* 4-byte pattern at [true_top - 4, true_top).  Detects an adjacent higher
 * slot's SP underflowing into our guard region. */
#define KSTACK_GUARD_TOP ((uint32_t)0xCAFECAFEu)

#ifdef KSTACK_USAGE_TRACK
#define KSTACK_PAINT ((uint8_t)0xA5u)
#define KSTACK_PAINT_HEADROOM 48u

static uint16_t kstack_hwm[PROC_MAX];
#endif

static inline uintptr_t kstack_slot_true_top(uint32_t slot_idx) {
  return (uintptr_t)__kstack_region_base + PROC_KSTACK_IDLE_SIZE +
         (uintptr_t)slot_idx * PROC_KSTACK_SIZE;
}

static inline uintptr_t kstack_slot_true_base(uint32_t slot_idx) {
  if (slot_idx == 0u) return (uintptr_t)__kstack_region_base;
  return (uintptr_t)__kstack_region_base + PROC_KSTACK_IDLE_SIZE +
         (uintptr_t)(slot_idx - 1u) * PROC_KSTACK_SIZE;
}

static inline uintptr_t kstack_slot_payload_base(uint32_t slot_idx) {
  return kstack_slot_true_base(slot_idx) + sizeof(uintptr_t);
}

static inline uintptr_t kstack_slot_payload_top(uint32_t slot_idx) {
  return kstack_slot_true_top(slot_idx) - PROC_KSTACK_GUARD_BYTES;
}

static void kstack_init_slot_impl(pcb_t *p, uint32_t slot_idx) {
  p->kernel_sp = kstack_slot_payload_top(slot_idx);
}

static void kstack_plant_canary_impl(uint32_t slot_idx) {
  uintptr_t base = kstack_slot_true_base(slot_idx);
  uintptr_t top = kstack_slot_true_top(slot_idx);
  *(volatile uintptr_t *)base = KSTACK_CANARY_BASE;
  *(volatile uint32_t *)(uintptr_t)(top - 4u) = KSTACK_GUARD_TOP;
}

static int kstack_slot_canary_ok(uint32_t slot_idx, uintptr_t *base_out,
                                 uintptr_t *got_base_out,
                                 uint32_t *got_guard_out) {
  uintptr_t base = kstack_slot_true_base(slot_idx);
  uintptr_t top = kstack_slot_true_top(slot_idx);
  uintptr_t got_base = *(volatile uintptr_t *)base;
  uint32_t got_guard = *(volatile uint32_t *)(uintptr_t)(top - 4u);

  if (base_out) *base_out = base;
  if (got_base_out) *got_base_out = got_base;
  if (got_guard_out) *got_guard_out = got_guard;

  return got_base == KSTACK_CANARY_BASE && got_guard == KSTACK_GUARD_TOP;
}

#ifdef KSTACK_USAGE_TRACK
static uint32_t kstack_current_slot(void) {
  pcb_t *cur = current;
  if (!cur) return PROC_MAX;
  for (uint32_t i = 0u; i < PROC_MAX; i++) {
    if (&proc_table[i] == cur) return i;
  }
  return PROC_MAX;
}

static uintptr_t kstack_current_sp(void) {
#if defined(__ia16__)
  uint16_t sp;
  __asm__ volatile("movw %%sp, %0" : "=r"(sp));
  return (uintptr_t)sp;
#elif defined(__arm__) || defined(__thumb__) || defined(__ARM_ARCH)
  uintptr_t sp;
  __asm__ volatile("mov %0, sp" : "=r"(sp));
  return sp;
#else
  return current ? current->kernel_sp : 0u;
#endif
}

static void kstack_paint_impl(void) {
  uint32_t slot = kstack_current_slot();
  if (slot >= PROC_MAX) return;

  uintptr_t base = kstack_slot_payload_base(slot);
  uintptr_t sp = kstack_current_sp();
  uintptr_t top = kstack_slot_payload_top(slot);
  if (sp <= base + KSTACK_PAINT_HEADROOM || top < sp) return;

  volatile uint8_t *p = (volatile uint8_t *)base;
  volatile uint8_t *end =
      (volatile uint8_t *)(uintptr_t)(sp - KSTACK_PAINT_HEADROOM);
  while (p < end) *p++ = KSTACK_PAINT;
}

static uint16_t kstack_scan_impl(void) {
  uint32_t slot = kstack_current_slot();
  if (slot >= PROC_MAX) return 0;

  uintptr_t base = kstack_slot_payload_base(slot);
  uintptr_t top = kstack_slot_payload_top(slot);
  volatile uint8_t *p = (volatile uint8_t *)base;
  volatile uint8_t *t = (volatile uint8_t *)top;
  while (p < t && *p == KSTACK_PAINT) p++;

  uint32_t used = (uint32_t)(top - (uintptr_t)p);
  if (used > kstack_hwm[slot]) {
    kstack_hwm[slot] = (uint16_t)used;
    return (uint16_t)used;
  }
  return 0;
}

static uint16_t kstack_capacity_impl(void) {
  uint32_t slot = kstack_current_slot();
  if (slot >= PROC_MAX) return 0;
  return (uint16_t)(kstack_slot_payload_top(slot) -
                    kstack_slot_payload_base(slot));
}

static void kstack_usage_report_impl(void) {
  for (uint32_t i = 0u; i < PROC_MAX; i++) {
    uint16_t cap =
        (uint16_t)(kstack_slot_payload_top(i) - kstack_slot_payload_base(i));
    mod_vfs.klogf("KSTACK: slot=%u  hwm=%u/%u bytes\n", (unsigned)i,
                  (unsigned)kstack_hwm[i], (unsigned)cap);
  }
}
#endif /* KSTACK_USAGE_TRACK */

#else /* PROC_HAS_FIXED_REGION_KSTACK */

static void kstack_init_slot_impl(pcb_t *p, uint32_t slot_idx) {
  (void)p;
  (void)slot_idx;
}

static void kstack_plant_canary_impl(uint32_t slot_idx) { (void)slot_idx; }

static int kstack_slot_canary_ok(uint32_t slot_idx, uintptr_t *base_out,
                                 uintptr_t *got_base_out,
                                 uint32_t *got_guard_out) {
  (void)slot_idx;
  if (base_out) *base_out = 0u;
  if (got_base_out) *got_base_out = 0u;
  if (got_guard_out) *got_guard_out = 0u;
  return 1;
}

#ifdef KSTACK_USAGE_TRACK
static void kstack_paint_impl(void) {}

static uint16_t kstack_scan_impl(void) { return 0; }

static uint16_t kstack_capacity_impl(void) { return 0; }

static void kstack_usage_report_impl(void) {}
#endif /* KSTACK_USAGE_TRACK */

#endif /* PROC_HAS_FIXED_REGION_KSTACK */

/* ── Public API ─────────────────────────────────────────────────────────── */

__attribute__((weak)) void proc_kstack_init_slot(pcb_t *p, uint32_t slot_idx) {
  kstack_init_slot_impl(p, slot_idx);
}

__attribute__((weak)) void proc_kstack_plant_canary(uint32_t slot_idx) {
  kstack_plant_canary_impl(slot_idx);
}

__attribute__((weak)) void proc_plant_kstack_canaries(void) {
  for (uint32_t i = 0u; i < PROC_MAX; i++) proc_kstack_plant_canary(i);
}

__attribute__((weak)) void proc_check_kstack_canary_panic(void) {
  for (uint32_t i = 0u; i < PROC_MAX; i++) {
    uintptr_t base;
    uintptr_t got_base;
    uint32_t got_guard;
    if (kstack_slot_canary_ok(i, &base, &got_base, &got_guard)) continue;

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

#ifdef KSTACK_USAGE_TRACK
void proc_kstack_paint(void) { kstack_paint_impl(); }

uint16_t proc_kstack_scan(void) { return kstack_scan_impl(); }

uint16_t proc_kstack_capacity(void) { return kstack_capacity_impl(); }

void proc_kstack_usage_report(void) { kstack_usage_report_impl(); }
#endif /* KSTACK_USAGE_TRACK */
