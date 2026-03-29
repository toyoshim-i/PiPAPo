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

#include "proc.h"

#include <stddef.h> /* NULL, offsetof */

#include "../common/errno.h"
#include "../klog.h"
#include "../mm/mem_region.h"
#include "../mm/page.h"  /* PAGE_SIZE — for proc_setup_stack */
#include "../common/spinlock.h" /* SPIN_PROC */
#include "arch/arch.h"   /* arch_build_initial_frame */
#include "arch/ioregs.h"
#include "sched.h" /* sched_get_ticks — for start_time */

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

void proc_init(void) {
  /* Zero all slots and mark them free */
  for (uint32_t i = 0u; i < PROC_MAX; i++) {
    __builtin_memset(&proc_table[i], 0, sizeof(pcb_t));
    proc_table[i].state = PROC_FREE;
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
  if (spin_have_hw())
    core_id_reg = (volatile uint32_t *)0xD0000000u;

  klogf(
      "PROC: process table  slots=%u"
      "  (pid 0 = kernel, pids 1-%u available)\n",
      (uint32_t)PROC_MAX, (uint32_t)(PROC_MAX - 1u));

#ifdef PPAP_TESTS
  /* Self-test: allocate a slot, verify it looks sane, then free it. */
  pcb_t *p = proc_alloc();

  uint32_t ok = (p != NULL) && (p->pid == 1) &&
                (p->state == PROC_FREE) &&
                (current == &proc_table[0]) &&
                (current->state == PROC_RUNNABLE);

  if (p) proc_free(p);

  /* Reset PID counter so the first real process gets PID 1. */
  next_pid = 1;

  klogf("PROC: self-test %s\n", ok ? "PASSED" : "FAILED");
#endif
}

pcb_t *proc_alloc(void) {
  uint32_t saved = spin_lock_irqsave(SPIN_PROC);
  pcb_t *result = NULL;

  /* Scan slots 1..PROC_MAX-1; slot 0 belongs to the kernel thread */
  for (uint32_t i = 1u; i < PROC_MAX; i++) {
    if (proc_table[i].state == PROC_FREE) {
      __builtin_memset(&proc_table[i], 0, sizeof(pcb_t));
      proc_table[i].pid = next_pid++;
      proc_table[i].umask_val = DEFAULT_UMASK;
      proc_table[i].running_on_core = -1;
      proc_table[i].start_time = sched_get_ticks();
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

int proc_track_page_range(pcb_t *p, uint32_t start_slot, page_id_t base_id,
                          uint32_t size) {
  uint32_t n_pages;

  if (!p || base_id == PAGE_ID_INVALID || size == 0) return 0;

  n_pages = (size + PAGE_SIZE - 1u) / PAGE_SIZE;
  if (start_slot > USER_PAGES_MAX || n_pages > USER_PAGES_MAX - start_slot)
    return -(int)ENOMEM;

  for (uint32_t i = 0; i < n_pages; i++)
    p->user_page_ids[start_slot + i] = (page_id_t)(base_id + i);
  return (int)n_pages;
}

int proc_track_page(pcb_t *p, uint32_t slot, page_id_t id) {
  if (!p || id == PAGE_ID_INVALID) return 0;
  if (slot >= USER_PAGES_MAX) return -(int)ENOMEM;
  p->user_page_ids[slot] = id;
  return 0;
}

uint32_t proc_first_page_backed_slot(const pcb_t *p) {
  if (!p) return USER_PAGES_MAX;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
    if (p->user_page_ids[i] != PAGE_ID_INVALID) return i;
  }
  return USER_PAGES_MAX;
}

static uint32_t proc_last_page_backed_slot(const pcb_t *p) {
  if (!p) return USER_PAGES_MAX;
  for (uint32_t i = USER_PAGES_MAX; i > 0; i--) {
    if (p->user_page_ids[i - 1] != PAGE_ID_INVALID) return i - 1;
  }
  return USER_PAGES_MAX;
}

page_id_t proc_page_backed_base(const pcb_t *p) {
  uint32_t slot = proc_first_page_backed_slot(p);
  if (slot >= USER_PAGES_MAX) return PAGE_ID_INVALID;
  return p->user_page_ids[slot];
}

page_id_t proc_last_page_backed_base(const pcb_t *p) {
  uint32_t slot = proc_last_page_backed_slot(p);
  if (slot >= USER_PAGES_MAX) return PAGE_ID_INVALID;
  return p->user_page_ids[slot];
}

uint32_t proc_page_backed_count(const pcb_t *p) {
  uint32_t slot;
  uint32_t count = 0;

  slot = proc_first_page_backed_slot(p);
  if (slot >= USER_PAGES_MAX) return 0;
  while (slot < USER_PAGES_MAX &&
         p->user_page_ids[slot] != PAGE_ID_INVALID) {
    count++;
    slot++;
  }
  return count;
}

uint32_t proc_tracked_page_count(const pcb_t *p) {
  uint32_t count = 0;

  if (!p) return 0;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
    if (p->user_page_ids[i] != PAGE_ID_INVALID) count++;
  }
  return count;
}

int proc_page_backed_contains(const pcb_t *p, uintptr_t addr) {
  if (!p) return 0;
#if defined(__ia16__)
  /* On i16, user pages are beyond near-pointer range; addr comparisons
   * don't work with 16-bit uintptr_t.  Use page_id-based lookup. */
  (void)addr;
  return 0;  /* TODO: implement via page_id range check */
#else
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
    uintptr_t base;
    if (p->user_page_ids[i] == PAGE_ID_INVALID) continue;
    base = (uintptr_t)mm_page_to_ptr(p->user_page_ids[i]);
    if (addr >= base && addr < base + PAGE_SIZE) return 1;
  }
  return 0;
#endif
}

void proc_clear_page_tracking(pcb_t *p) {
  if (!p) return;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) p->user_page_ids[i] = PAGE_ID_INVALID;
}

void proc_copy_page_tracking(pcb_t *dst, const pcb_t *src) {
  if (!dst || !src) return;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++)
    dst->user_page_ids[i] = src->user_page_ids[i];
}

void proc_copy_page_tracking_to_array(const pcb_t *src,
                                      page_id_t dst[USER_PAGES_MAX]) {
  if (!src || !dst) return;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) dst[i] = src->user_page_ids[i];
}

void proc_restore_page_tracking_from_array(pcb_t *dst,
                                           const page_id_t src[USER_PAGES_MAX]) {
  if (!dst || !src) return;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) dst->user_page_ids[i] = (page_id_t)(uintptr_t)src[i];
}

void proc_release_tracked_pages(pcb_t *p, uint32_t start_slot,
                                uint32_t end_slot) {
  if (!p) return;
  if (start_slot >= USER_PAGES_MAX) return;
  if (end_slot > USER_PAGES_MAX) end_slot = USER_PAGES_MAX;
  for (uint32_t i = start_slot; i < end_slot; i++) {
    if (p->user_page_ids[i] == PAGE_ID_INVALID) continue;
    mm_page_free(p->user_page_ids[i]);
    p->user_page_ids[i] = PAGE_ID_INVALID;
  }
}

void proc_release_private_tracked_pages(pcb_t *p, const pcb_t *shared_owner) {
  if (!p || !shared_owner) return;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
    if (p->user_page_ids[i] == PAGE_ID_INVALID) continue;
    if (p->user_page_ids[i] == shared_owner->user_page_ids[i]) continue;
    mm_page_free(p->user_page_ids[i]);
    p->user_page_ids[i] = PAGE_ID_INVALID;
  }
}

void proc_release_tracked_pages_from_array(page_id_t pages[USER_PAGES_MAX]) {
  if (!pages) return;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
    if (pages[i] == PAGE_ID_INVALID) continue;
    mm_page_free(pages[i]);
  }
}

void proc_release_private_tracked_pages_from_array(
    page_id_t pages[USER_PAGES_MAX], const page_id_t shared[USER_PAGES_MAX]) {
  if (!pages || !shared) return;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
    if (pages[i] == PAGE_ID_INVALID) continue;
    if (pages[i] == shared[i]) continue;
    mm_page_free(pages[i]);
  }
}

void proc_setup_stack(pcb_t *p, void (*entry)(void), uintptr_t user_sp) {
  uint32_t *sp;

#if defined(__riscv)
  /* RISC-V mscratch split: kernel frame on stack_page, user_sp in TF_USER_SP */
  sp = (uint32_t *)((uint8_t *)mm_page_to_ptr(p->stack_page_id) + PAGE_SIZE);
  sp = arch_build_initial_frame(sp, entry);
  /* TF_USER_SP at word offset 32 (byte offset 128) */
  sp[32] = user_sp ? (uint32_t)user_sp
                   : (uint32_t)(uintptr_t)mm_page_to_ptr(p->stack_page_id) + PAGE_SIZE;
  p->sp = (uint32_t)(uintptr_t)sp;
  p->kernel_sp = (uint32_t)(uintptr_t)mm_page_to_ptr(p->stack_page_id) + PAGE_SIZE;
#elif defined(__m68k__)
  sp = (uint32_t *)((uint8_t *)mm_page_to_ptr(p->stack_page_id) + PAGE_SIZE);
  sp = arch_build_initial_frame(sp, entry);
  p->sp = (uint32_t)(uintptr_t)sp;
#else
  if (user_sp)
    sp = (uint32_t *)(void *)user_sp;
  else
    sp = (uint32_t *)((uint8_t *)mm_page_to_ptr(p->stack_page_id) + PAGE_SIZE);
  sp = arch_build_initial_frame(sp, entry);
  p->sp = (uint32_t)(uintptr_t)sp;
#endif

  p->ticks_remaining = PROC_DEFAULT_TICKS;
}
