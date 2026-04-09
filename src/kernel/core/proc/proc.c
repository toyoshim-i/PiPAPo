/*
 * proc.c — Process table and PCB lifecycle
 *
 * Manages a flat array of PROC_MAX process control blocks.  proc_table[0]
 * is permanently reserved for the initial kernel/init thread; slots 1..7
 * are available for user processes allocated via proc_alloc().
 *
 * proc_alloc() scans for a PROC_FREE slot — O(PROC_MAX) = O(8), negligible.
 * PIDs are assigned from a monotonically increasing counter so they are
 * unique for the lifetime of the kernel (no wraparound in Phase 1).
 */

#include "kernel/core/proc/proc.h"

#include <stddef.h> /* NULL, offsetof */

#include "common/errno.h"
#include "kernel/common/ioregs.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/common/spinlock.h" /* SPIN_PROC */
#include "kernel/core/arch.h"       /* arch_build_initial_frame */
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/mm/page.h"    /* PAGE_SIZE — for proc_setup_stack */
#include "kernel/core/proc/sched.h" /* sched_get_ticks — for start_time */

/* Default file creation mask (octal 022 → owner rw, group/other r) */
#define DEFAULT_UMASK 022

/* Verify PCB_SP_OFFSET matches the actual struct layout at compile time.
 * If this fires, update PCB_SP_OFFSET in proc.h. */
_Static_assert(
    offsetof(pcb_t, sp) == PCB_SP_OFFSET,
    "PCB_SP_OFFSET does not match offsetof(pcb_t, sp) — update proc.h");

/* ── Globals ─────────────────────────────────────────────────────────────── */

pcb_t proc_table[PROC_MAX];
pcb_t *current_core[2] = {NULL, NULL};

/* Indirect core-ID register pointer for assembly (switch.S, svc.S).
 * Points to SIO_CPUID on RP2040, or core_id_zero on QEMU.
 * Assembly does: ldr rN, =core_id_reg; ldr rN, [rN]; ldr rN, [rN]
 * → two loads, no branches, works on both platforms. */
static uint32_t core_id_zero = 0;
volatile uint32_t *core_id_reg = &core_id_zero;

/* Monotonically increasing PID counter.  Starts at 1; pid 0 is the kernel. */
static pid_t next_pid = 1;

/* ── Public API ──────────────────────────────────────────────────────────── */

#if defined(__ia16__)
/* Per-process kernel stack canaries.  Slots are 2 KB each, packed
 * adjacent at 0xE000..0xFFFF.  Two sentinels per slot:
 *
 *   - BASE canary at slot's lowest address — detects this slot's own
 *     SP underrunning past its base.
 *   - TOP guard (4 bytes / 2 words) at slot's highest addresses, just
 *     below true_top.  PCB.kernel_stack_top is set to (true_top - 4),
 *     so the slot's own stack never reaches the guard region.  This
 *     detects the case where the *adjacent higher* slot's SP
 *     underflowed past its base and wrote into our top region — which
 *     is invisible to a base-only canary because the writes never
 *     reach our base. */
#define I16_KSTACK_CANARY ((uint16_t)0xCA57u)
#define I16_KSTACK_GUARD ((uint16_t)0xCAFEu)
#define I16_KSTACK_GUARD_BYTES 4u
#define I16_KSTACK_TRUE_BASE(i) ((uint16_t)(0xE000u + (uint16_t)(i) * 2048u))
#define I16_KSTACK_TRUE_TOP(i) \
  ((uint16_t)(0xE000u + (uint16_t)((i) + 1u) * 2048u))

#define I16_KSTACK_BASE(i) I16_KSTACK_TRUE_BASE(i)

static void i16_kstack_plant_canary(uint32_t i) {
  *(volatile uint16_t *)(uintptr_t)I16_KSTACK_BASE(i) = I16_KSTACK_CANARY;
  uint16_t top = I16_KSTACK_TRUE_TOP(i);
  *(volatile uint16_t *)(uintptr_t)(uint16_t)(top - 4u) = I16_KSTACK_GUARD;
  *(volatile uint16_t *)(uintptr_t)(uint16_t)(top - 2u) = I16_KSTACK_GUARD;
}
#endif

void proc_init(void) {
  /* Zero all slots and mark them free */
  for (uint32_t i = 0u; i < PROC_MAX; i++) {
    __builtin_memset(&proc_table[i], 0, sizeof(pcb_t));
    proc_table[i].state = PROC_FREE;
    proc_table[i].stack_page_id = PAGE_ID_INVALID;
#if defined(__ia16__)
    proc_table[i].kernel_stack_top =
        (uint16_t)(I16_KSTACK_TRUE_TOP(i) - I16_KSTACK_GUARD_BYTES);
    i16_kstack_plant_canary(i);
#endif
    proc_table[i].clear_child_tid = user_page_ref_invalid();
    for (uint32_t j = 0; j < USER_PAGES_MAX; j++)
      proc_table[i].user_pages[j] = PAGE_ID_INVALID;
  }

  /* Pre-initialise slot 0 as the initial kernel thread.
   * stack_page is NULL: this thread runs on the initial kernel stack
   * set up by startup.S; no extra stack allocation is needed. */
  proc_table[0].pid = 0;
  proc_table[0].ppid = 0;
  proc_table[0].state = PROC_RUNNABLE;
  proc_table[0].ticks_remaining = PROC_DEFAULT_TICKS;
  proc_table[0].is_idle = 1;
  __builtin_memcpy(proc_table[0].comm, "kernel", 7);

  current_core[0] = &proc_table[0];

  /* Point assembly's core_id_reg at the SIO_CPUID register on RP2040.
   * On QEMU (no SIO), it stays pointing at core_id_zero → always 0. */
  if (spin_have_hw()) core_id_reg = (volatile uint32_t *)0xD0000000u;

  mod_vfs.klogf(
      "PROC: process table  slots=%u"
      "  (pid 0 = kernel, pids 1-%u available)\n",
      (unsigned)PROC_MAX, (unsigned)(PROC_MAX - 1u));

#ifdef PPAP_TESTS
  /* Self-test: allocate a slot, verify it looks sane, then free it. */
  pcb_t *p = proc_alloc();

  uint32_t ok = (p != NULL) && (p->pid == 1) && (p->state == PROC_FREE) &&
                (current == &proc_table[0]) &&
                (current->state == PROC_RUNNABLE);

  if (p) proc_free(p);

  /* Reset PID counter so the first real process gets PID 1. */
  next_pid = 1;

  mod_vfs.klogf("PROC: self-test %s\n", ok ? "PASSED" : "FAILED");
#endif
}

#if defined(__ia16__)
/* Walk every PCB slot and check that the kernel-stack canary at slot
 * base is still intact.  Any mismatch is a kernel-stack overrun and
 * is unrecoverable: panic with the offending slot, the current
 * process's pid and comm, and the clobber value, then halt forever.
 *
 * Called from end-of-syscall and end-of-vfork-restore paths so a
 * stack overrun is caught at the next kernel→user transition rather
 * than corrupting subsequent state silently. */
void proc_check_kstack_canary_panic(void) {
  for (uint32_t i = 0u; i < PROC_MAX; i++) {
    uint16_t base = I16_KSTACK_BASE(i);
    uint16_t top = I16_KSTACK_TRUE_TOP(i);
    uint16_t got_base = *(volatile uint16_t *)(uintptr_t)base;
    uint16_t got_g0 = *(volatile uint16_t *)(uintptr_t)(uint16_t)(top - 4u);
    uint16_t got_g1 = *(volatile uint16_t *)(uintptr_t)(uint16_t)(top - 2u);
    if (got_base == I16_KSTACK_CANARY && got_g0 == I16_KSTACK_GUARD &&
        got_g1 == I16_KSTACK_GUARD)
      continue;
    pcb_t *cur = current;
    mod_vfs.klogf(
        "PANIC: kernel stack overrun  slot=%u base=%x got_base=%x"
        "  topG0=%x topG1=%x  pid=%u comm=%s\n",
        (unsigned)i, (unsigned)base, (unsigned)got_base, (unsigned)got_g0,
        (unsigned)got_g1, cur ? (unsigned)cur->pid : 0u,
        cur ? cur->comm : "(null)");
    for (;;) arch_wfi();
  }
}
#endif

pcb_t *proc_alloc(void) {
  uint32_t saved = spin_lock_irqsave(SPIN_PROC);
  pcb_t *result = NULL;

  /* Scan slots 1..PROC_MAX-1; slot 0 belongs to the kernel thread. */
  for (uint32_t i = 1u; i < PROC_MAX; i++) {
    if (proc_table[i].state == PROC_FREE) {
      __builtin_memset(&proc_table[i], 0, sizeof(pcb_t));
      proc_table[i].pid = next_pid++;
      proc_table[i].stack_page_id = PAGE_ID_INVALID;
#if defined(__ia16__)
      proc_table[i].kernel_stack_top =
          (uint16_t)(I16_KSTACK_TRUE_TOP(i) - I16_KSTACK_GUARD_BYTES);
      i16_kstack_plant_canary(i);
#endif
      proc_table[i].clear_child_tid = user_page_ref_invalid();
      for (uint32_t j = 0; j < USER_PAGES_MAX; j++)
        proc_table[i].user_pages[j] = PAGE_ID_INVALID;
      proc_table[i].umask_val = DEFAULT_UMASK;
      proc_table[i].running_on_core = -1;
      proc_table[i].start_time = sched_get_ticks();
      /* Init fd_map to "no descriptor" (0 is a valid desc_id) */
      for (int _f = 0; _f < FD_MAX; _f++)
        proc_table[i].fd_map[_f] = FD_DESC_NONE;
      result = &proc_table[i];
      break;
    }
  }

  spin_unlock_irqrestore(SPIN_PROC, saved);
  return result;
}

void proc_free(pcb_t *p) {
  if (!p) return;
  uint32_t saved = spin_lock_irqsave(SPIN_PROC);
  p->state = PROC_FREE;
  spin_unlock_irqrestore(SPIN_PROC, saved);
}

int proc_track_page_range(pcb_t *p, uint32_t start_slot, page_id_t base_page_id,
                          uint32_t n_pages) {
  if (!p || base_page_id == PAGE_ID_INVALID || n_pages == 0) return 0;

  if (start_slot > USER_PAGES_MAX || n_pages > USER_PAGES_MAX - start_slot)
    return -(int)ENOMEM;

  for (uint32_t i = 0; i < n_pages; i++)
    p->user_pages[start_slot + i] = base_page_id + (page_id_t)i;
  return (int)n_pages;
}

int proc_track_page(pcb_t *p, uint32_t slot, page_id_t page_id) {
  if (!p || page_id == PAGE_ID_INVALID) return 0;
  if (slot >= USER_PAGES_MAX) return -(int)ENOMEM;
  p->user_pages[slot] = page_id;
  return 0;
}

uint32_t proc_first_page_backed_slot(const pcb_t *p) {
  if (!p) return USER_PAGES_MAX;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
    if (p->user_pages[i] != PAGE_ID_INVALID) return i;
  }
  return USER_PAGES_MAX;
}

static uint32_t proc_last_page_backed_slot(const pcb_t *p) {
  if (!p) return USER_PAGES_MAX;
  for (uint32_t i = USER_PAGES_MAX; i > 0; i--) {
    if (p->user_pages[i - 1] != PAGE_ID_INVALID) return i - 1;
  }
  return USER_PAGES_MAX;
}

page_id_t proc_page_backed_base(const pcb_t *p) {
  uint32_t slot = proc_first_page_backed_slot(p);
  if (slot >= USER_PAGES_MAX) return PAGE_ID_INVALID;
  return p->user_pages[slot];
}

page_id_t proc_last_page_backed_base(const pcb_t *p) {
  uint32_t slot = proc_last_page_backed_slot(p);
  if (slot >= USER_PAGES_MAX) return PAGE_ID_INVALID;
  return p->user_pages[slot];
}

uint32_t proc_page_backed_count(const pcb_t *p) {
  uint32_t slot;
  uint32_t count = 0;

  slot = proc_first_page_backed_slot(p);
  if (slot >= USER_PAGES_MAX) return 0;
  while (slot < USER_PAGES_MAX && p->user_pages[slot] != PAGE_ID_INVALID) {
    count++;
    slot++;
  }
  return count;
}

uint32_t proc_tracked_page_count(const pcb_t *p) {
  uint32_t count = 0;

  if (!p) return 0;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
    if (p->user_pages[i] != PAGE_ID_INVALID) count++;
  }
  return count;
}

int proc_page_backed_contains(const pcb_t *p, uintptr_t addr) {
  if (!p) return 0;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
    uint32_t base;
    if (p->user_pages[i] == PAGE_ID_INVALID) continue;
    base = mem_region_page_linear(p->user_pages[i]);
    if (addr >= base && addr < base + PAGE_SIZE) return 1;
  }
  return 0;
}

void proc_clear_page_tracking(pcb_t *p) {
  if (!p) return;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++)
    p->user_pages[i] = PAGE_ID_INVALID;
}

void proc_copy_page_tracking(pcb_t *dst, const pcb_t *src) {
  if (!dst || !src) return;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++)
    dst->user_pages[i] = src->user_pages[i];
}

void proc_copy_page_tracking_to_array(const pcb_t *src,
                                      page_id_t dst[USER_PAGES_MAX]) {
  if (!src || !dst) return;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) dst[i] = src->user_pages[i];
}

void proc_restore_page_tracking_from_array(
    pcb_t *dst, const page_id_t src[USER_PAGES_MAX]) {
  if (!dst || !src) return;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) dst->user_pages[i] = src[i];
}

void proc_release_tracked_pages(pcb_t *p, uint32_t start_slot,
                                uint32_t end_slot) {
  if (!p) return;
  if (start_slot >= USER_PAGES_MAX) return;
  if (end_slot > USER_PAGES_MAX) end_slot = USER_PAGES_MAX;
  for (uint32_t i = start_slot; i < end_slot; i++) {
    if (p->user_pages[i] == PAGE_ID_INVALID) continue;
#if defined(__ia16__)
    /* i16 owns one whole exec-time segment via image.data. user_pages[]
     * is occupancy bookkeeping only, so clearing slots must not free the
     * underlying page. */
    p->user_pages[i] = PAGE_ID_INVALID;
#else
    mem_region_free_tracked_page_id(p->user_pages[i]);
    p->user_pages[i] = PAGE_ID_INVALID;
#endif
  }
}

void proc_release_private_tracked_pages(pcb_t *p, const pcb_t *shared_owner) {
  if (!p || !shared_owner) return;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
    if (p->user_pages[i] == PAGE_ID_INVALID) continue;
    if (p->user_pages[i] == shared_owner->user_pages[i]) continue;
#if defined(__ia16__)
    p->user_pages[i] = PAGE_ID_INVALID;
#else
    mem_region_free_tracked_page_id(p->user_pages[i]);
    p->user_pages[i] = PAGE_ID_INVALID;
#endif
  }
}

void proc_release_tracked_pages_from_array(page_id_t pages[USER_PAGES_MAX]) {
  if (!pages) return;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
    if (pages[i] == PAGE_ID_INVALID) continue;
#if !defined(__ia16__)
    mem_region_free_tracked_page_id(pages[i]);
#endif
  }
}

void proc_release_private_tracked_pages_from_array(
    page_id_t pages[USER_PAGES_MAX], const page_id_t shared[USER_PAGES_MAX]) {
  if (!pages || !shared) return;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
    if (pages[i] == PAGE_ID_INVALID) continue;
    if (pages[i] == shared[i]) continue;
#if !defined(__ia16__)
    mem_region_free_tracked_page_id(pages[i]);
#endif
  }
}

void proc_setup_stack(pcb_t *p, void (*entry)(void), uintptr_t user_sp) {
  uint32_t *sp;

  {
#if defined(__ia16__)
    /* i16: stack_base not used — frame is built at user_sp or via
     * mem_region_page_write.  arch_build_initial_frame handles it. */
    if (user_sp)
      sp = (uint32_t *)(void *)user_sp;
    else {
      /* TODO: i16 proc_setup_stack without user_sp */
      p->ticks_remaining = PROC_DEFAULT_TICKS;
      return;
    }
    sp = arch_build_initial_frame(sp, entry);
    p->sp = (uint32_t)(uintptr_t)sp;
#else
    uint8_t *stack_base = (uint8_t *)mem_region_page_to_ptr(p->stack_page_id);
#endif

#if !defined(__ia16__)
#if defined(__riscv)
    /* RISC-V mscratch split: kernel frame on stack_page, user_sp in TF_USER_SP
     */
    sp = (uint32_t *)(stack_base + PAGE_SIZE);
    sp = arch_build_initial_frame(sp, entry);
    /* TF_USER_SP at word offset 32 (byte offset 128) */
    sp[32] = user_sp ? (uint32_t)user_sp
                     : (uint32_t)(uintptr_t)stack_base + PAGE_SIZE;
    p->sp = (uint32_t)(uintptr_t)sp;
    p->kernel_sp = (uint32_t)(uintptr_t)stack_base + PAGE_SIZE;
#elif defined(__m68k__)
    sp = (uint32_t *)(stack_base + PAGE_SIZE);
    sp = arch_build_initial_frame(sp, entry);
    p->sp = (uint32_t)(uintptr_t)sp;
#else
    if (user_sp)
      sp = (uint32_t *)(void *)user_sp;
    else
      sp = (uint32_t *)(stack_base + PAGE_SIZE);
    sp = arch_build_initial_frame(sp, entry);
    p->sp = (uint32_t)(uintptr_t)sp;
#endif
#endif /* !__ia16__ */
  }

  p->ticks_remaining = PROC_DEFAULT_TICKS;
}
