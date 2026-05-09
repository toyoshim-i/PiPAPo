/*
 * proc.h — Process function declarations (core-only)
 *
 * Data types (pcb_t, proc_state_t, proc_table, current) are in
 * kernel/common/core/proc_info.h.  This header adds core-only function
 * declarations and the arch-dependent proc_user_ptr_to_page_ref() inline.
 *
 * VFS code should include proc_info.h instead of this header.
 */

#ifndef PPAP_KERNEL_CORE_PROC_PROC_H
#define PPAP_KERNEL_CORE_PROC_PROC_H

#include "kernel/common/core/proc_info.h"
#include "kernel/core/arch.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/proc/kstack.h"

/* ── API ──────────────────────────────────────────────────────────────────────
 */

/*
 * Initialise the process table.
 * - Clears all slots and marks them PROC_FREE.
 * - Pre-initialises proc_table[0] as the initial kernel thread (pid 0,
 *   PROC_RUNNABLE) and sets current = &proc_table[0].
 * - Runs a brief self-test and prints the result over UART.
 * Must be called once from kmain() after UART and mm are ready.
 */
void proc_init(void);

/*
 * Allocate a free PCB slot (slots 1..PROC_MAX-1).
 * Clears the slot, assigns a unique pid, and returns a pointer to it.
 * Returns NULL if all slots are in use.
 * The caller is responsible for setting state, stack_page, and any other
 * fields before making the process PROC_RUNNABLE.
 */
pcb_t *proc_alloc(void);

/*
 * Release a PCB slot.
 * Marks the PCB PROC_FREE; the caller must have already freed any pages
 * held by the process (stack_page, user_pages[]) before calling this.
 * No-op if p is NULL.
 */
void proc_free(pcb_t *p);

/*
 * Track page-backed user memory in user_pages[].
 * Returns the number of tracked pages, or -ENOMEM if the range would
 * exceed USER_PAGES_MAX.
 */
int proc_track_page_range(pcb_t *p, uint32_t start_slot, page_id_t base_page_id,
                          uint32_t n_pages);

/*
 * Track one page-backed user page in user_pages[].
 * Returns 0 on success, or -ENOMEM if the slot is out of range.
 */
int proc_track_page(pcb_t *p, uint32_t slot, page_id_t page_id);

/* Return the first tracked page-backed page ID, or PAGE_ID_INVALID. */
page_id_t proc_page_backed_base(const pcb_t *p);

/* Resolve a raw user pointer to a page+offset reference for process p.
 *
 * i16 needs base_page (DS-relative addressing), so we gate on it.
 * Flat-memory architectures (ARM, m68k, RISC-V) resolve addresses
 * directly and don't need tracked pages to exist. */
static inline int proc_user_ptr_to_page_ref(const pcb_t *p, uintptr_t user_ptr,
                                            user_page_ref_t *ref) {
  page_id_t base_page;

  if (!p || !ref) return -1;
  base_page = proc_page_backed_base(p);
#if defined(__ia16__)
  if (base_page == PAGE_ID_INVALID) {
    *ref = user_page_ref_invalid();
    return -1;
  }
#endif
  ref->page = arch_user_ptr_to_page(base_page, user_ptr, &ref->off);
  if (ref->page == PAGE_ID_INVALID) {
    *ref = user_page_ref_invalid();
    return -1;
  }
  return 0;
}

/* Return the last tracked page-backed page ID, or PAGE_ID_INVALID. */
page_id_t proc_last_page_backed_base(const pcb_t *p);

/* Count contiguous tracked page-backed pages from the first tracked slot. */
uint32_t proc_page_backed_count(const pcb_t *p);

/* Return the slot index of the first tracked page, or USER_PAGES_MAX. */
uint32_t proc_first_page_backed_slot(const pcb_t *p);

/* Count all tracked page-backed pages regardless of slot layout. */
uint32_t proc_tracked_page_count(const pcb_t *p);

/* Return nonzero if addr is inside any tracked page-backed user page. */
int proc_page_backed_contains(const pcb_t *p, uintptr_t addr);

/* Clear page-backed tracking slots without freeing underlying pages. */
void proc_clear_page_tracking(pcb_t *p);

/* Copy tracked page slots from one process to another. */
void proc_copy_page_tracking(pcb_t *dst, const pcb_t *src);

/* Save tracked page slots from process into an array snapshot. */
void proc_copy_page_tracking_to_array(const pcb_t *src,
                                      page_id_t dst[USER_PAGES_MAX]);

/* Restore tracked page slots for process from an array snapshot. */
void proc_restore_page_tracking_from_array(pcb_t *dst,
                                           const page_id_t src[USER_PAGES_MAX]);

/* Free tracked pages in [start_slot, end_slot) and clear the slots. */
void proc_release_tracked_pages(pcb_t *p, uint32_t start_slot,
                                uint32_t end_slot);

/* Free tracked pages in p that are not shared with shared_owner. */
void proc_release_private_tracked_pages(pcb_t *p, const pcb_t *shared_owner);

/* Free all tracked pages recorded in an array snapshot. */
void proc_release_tracked_pages_from_array(page_id_t pages[USER_PAGES_MAX]);

/* Free pages in snapshot array that are not shared with shared[] slots. */
void proc_release_private_tracked_pages_from_array(
    page_id_t pages[USER_PAGES_MAX], const page_id_t shared[USER_PAGES_MAX]);

/*
 * Set up an initial kernel stack frame for a new process so that
 * PendSV_Handler can restore it on the first context switch.
 *
 * Pre-condition: p->stack_page_id must already reference a 4 KB stack backing
 * page on architectures that use one.  RISC-V instead builds the initial trap
 * frame on p->kernel_sp.  After this call p->sp is set and the process is
 * ready to be made PROC_RUNNABLE.
 *
 * On entry to `entry`, all callee-saved registers are zero, r0-r3 are zero,
 * and lr = 0xFFFFFFFD (EXC_RETURN: Thread mode, PSP, basic frame).
 *
 * user_sp: the PSP value after the hardware frame pop.  For plain kernel
 * threads pass 0 (defaults to stack_page + PAGE_SIZE).  For exec'd
 * processes this points to the argc slot built by execve().
 */
void proc_setup_stack(pcb_t *p, void (*entry)(void), uintptr_t user_sp);

/*
 * Build the initial stack frame for a new process so that the
 * context-switch path can restore it on first schedule.
 * Provided by each architecture (switch.S / arch_common.c).
 */
uint32_t *arch_build_initial_frame(uint32_t *sp, void (*entry)(void));

#endif /* PPAP_KERNEL_CORE_PROC_PROC_H */
