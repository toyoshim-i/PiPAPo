# i16 Per-Process 64 KB Segment Partition

> **Status**: Proposal.  Targets the kernel-stack overrun and shared-user-stack
> bug class on `pcxt` exposed during the `mem_region` Phase 1 work
> (commit `c113c49`, "store vfork saved frame below trap_ksp, not above").

## 1. Background

PPAP's i16 (`pcxt`) port runs user processes in **tiny model**: `CS = DS = SS
= ES = proc_seg`.  Each process is built as a single ELF that fits in a
64 KB segment.  Today the kernel allocates **only the minimum number of
4 KB pages** required for `mem_end + ELF16_STACK_SIZE` from the global
page pool, sized per-binary:

```text
init.elf  →  1 page  (4 KB)   text+data+bss+stack
sh.elf    →  6 pages (24 KB)  text+data+bss+stack
```

This is memory-efficient but creates several structural problems:

1. **Stack vs allocator interaction.**  The single-page allocator at
   [`page.c::page_alloc`](../../src/kernel/core/mm/page.c) picks the
   *highest-address* free page (top-down), to leave low contiguous
   space for ELF images.  On i16 the page pool spans 0x22000-0x9EFFF
   (500 KB), so a single-page allocation can return a page far above
   the user process's segment-relative reach (`proc_seg << 4 + 64 KB
   ≤ ~0x32000` for the first process).  The page is unreachable from
   the user's segment registers.  Latent today (no current pcxt user
   binary calls `mmap` or grows brk via the single-page path), but
   it's a guaranteed bug as soon as anyone does.

2. **Vfork-shared user stack.**  Vfork on i16 leaves the child sharing
   the parent's `proc_seg` until execve.  The kernel must save the
   parent's GP+IRET frame somewhere safe before yielding to the child,
   and restore it when the parent resumes.  The current implementation
   stores it at `trap_ksp - 24` on the parent's own kernel stack
   (commit `c113c49`).  This works but is fragile — any change to the
   trap frame layout, kernel stack geometry, or execve cleanup risks
   breaking it.

3. **execve cleanup is delicate.**  When the vfork child execve's a
   new binary, the kernel must:
   - Allocate fresh contiguous pages for the new ELF.
   - Track which old pages were *shared* with the parent (must not
     free) vs *child-private* (must free).
   - Build the new user stack frame inside the new pages.
   - Patch `exec_pending` for trap.S to switch SS:SP after returning.

   The vfork-private accounting in
   [`proc_release_private_tracked_pages_from_array`](../../src/kernel/core/proc/proc.c)
   exists solely to support this case.

4. **mmap is not implementable** in any meaningful way today: a
   user-space `mmap(NULL, 4096, ...)` would call the single-page
   allocator and receive a page outside the process's segment reach.
   The PPAP user stack of `mmap`-aware musl libc cannot run on i16.

5. **Address arithmetic in `i16_vfork_restore_frame`** is non-trivial
   because it must compute `(user_ss * 16 + user_sp) - data_base` to
   find the user stack page within the parent's variable-size
   allocation.  If `image.data.base_page` is wrong (e.g. for a vfork
   child whose `image` was never copied from the parent), the math
   silently writes to the wrong page.

The cleanest structural fix to all of these is to **give every user
process a fixed, pre-reserved 64 KB partition of conventional memory**
from boot.  This trades a small amount of memory for a large amount
of model simplicity.

---

## 2. Goals

- **Eliminate the segment-unreachable allocation class** by ensuring
  every page a user process can ever own is inside a single contiguous
  64 KB region addressable from its `proc_seg`.
- **Make `mmap` trivially implementable** as in-segment range tracking
  with no global allocator interaction.
- **Simplify `sys_brk`, `sys_mmap2`, `sys_munmap`, and `execve`** on
  i16 by removing dynamic page allocation from these paths.
- **Reduce the bug surface around vfork** by making post-execve
  segment isolation hardware-enforced (different `proc_seg` →
  unreachable from the parent's segment).

Non-goals:

- Eliminating the saved-frame dance for vfork's *pre-execve* window.
  As long as vfork keeps the "child shares parent's segment until
  execve" semantic (which we want for `mem_region_alloc`-free
  process spawning), the kernel still needs to save the parent's
  user GP frame somewhere safe across the yield.  Commit `c113c49`
  remains correct and necessary.  See §6 (alternative B) for the
  variant that drops this too.

---

## 3. Design

### 3.1 Per-slot user partition table

Add a static table indexed by user-process slot number:

```c
/* In src/target/pcxt/kernel/core/proc_layout.c (new) */

#include "kernel/common/config.h"

/* User-process segment partitions.  Indexed by user slot
 * (slot 0 of this table = process slot 1, the first user process,
 * because PCB slot 0 is the idle/kernel thread and has no user
 * segment).
 *
 * Each entry is a base linear address.  proc_seg = base >> 4.
 * Each partition is exactly 64 KB (16 pages × 4 KB).
 *
 * Layout (with PROC_MAX = 3, so 2 user slots):
 *   user slot 0 (PCB slot 1): 0x22000 .. 0x31FFF  (proc_seg = 0x2200)
 *   user slot 1 (PCB slot 2): 0x32000 .. 0x41FFF  (proc_seg = 0x3200)
 *
 * Total: 128 KB, well within the available conventional memory
 * (page pool currently has 500 KB).
 */
#define I16_USER_SLOT_BYTES (16u * PAGE_SIZE)  /* 64 KB */
#define I16_USER_SLOT_BASE_LINEAR(i) \
  (0x22000u + (uint32_t)(i) * I16_USER_SLOT_BYTES)

#define I16_USER_SLOTS (PROC_MAX - 1u)  /* exclude pid 0 idle */
```

These addresses align with paragraph (16-byte) boundaries, so they
form valid `proc_seg` values: `0x2200`, `0x3200`.

### 3.2 Page-pool reservation

The kernel boot path reserves the user-partition region from the
global page pool.  Current pcxt page pool starts at `0x22000`; the
new layout reserves the first `I16_USER_SLOTS * 16` pages for user
partitions and lets the page pool start above that:

```text
0x22000-0x31FFF  user partition 0 (16 pages)  reserved
0x32000-0x41FFF  user partition 1 (16 pages)  reserved
0x42000-0x9EFFF  page pool (kernel use only)  ~372 KB
```

`mem_region_page_alloc` on i16 only operates above `0x42000`, so it
can never return a page that's part of any user partition.  Kernel-
side allocations (file scratch buffers, FS metadata pools, etc.)
all come from this region.

### 3.3 PCB additions

Each user PCB gains an `i16_user_slot` field that is set on
`proc_alloc` and cleared on `proc_free`:

```c
#if defined(__ia16__)
  uint8_t i16_user_slot;  /* 0..I16_USER_SLOTS-1, or 0xFF if unowned */
#endif
```

`proc_alloc` finds a free user slot (linear scan over PCBs), assigns
`i16_user_slot`, and pre-fills the new PCB's segment-related fields:

```c
  uint8_t slot = i16_alloc_user_slot();
  if (slot == 0xFF) { proc_free(p); return NULL; }
  p->i16_user_slot = slot;
  uint32_t base_linear = I16_USER_SLOT_BASE_LINEAR(slot);
  p->image.data.base_page = (page_id_t)(base_linear / PAGE_SIZE);
```

### 3.4 execve / loader changes

`elf16_load_vnode` no longer allocates pages.  Instead, it:

1. Reads `current->i16_user_slot` and computes the base linear
   address and `proc_seg` from the table.
2. Zeros the entire 64 KB partition.
3. Loads the ELF PT_LOAD segments into the partition via
   `mem_region_page_write` exactly as today (the page IDs are now
   known statically from the slot).
4. Builds argc/argv at `user_sp_top = 0xFFE` (the top of the
   segment, leaving 2 bytes for alignment).
5. Sets `image.data.base_page` and `image.entry`.

`mem_region_page_alloc_contiguous` is no longer called from
`elf16_load_vnode`.  No more `npages > 16` failure.  No more
fragmentation risk.

Image-size limit becomes a hard 64 KB minus stack reservation,
which matches the existing `ELF16_MAX_SIZE = 60 KB` constant.

### 3.5 vfork

The pre-execve window is unchanged: child shares the parent's
segment, kernel saves the parent's GP frame at `trap_ksp - 24`,
seeds the child's kernel stack with `[parent_user_sp,
parent_user_ss]`, patches `AX = 0` in the shared user stack frame.

The crucial change is **post-execve**: when the vfork child calls
execve, it loads the new binary into **its own user slot's
segment**, not the parent's.  The child's `proc_seg` becomes
different from the parent's, and after the next iret the child
runs with `CS = DS = SS = ES = child_proc_seg`.  The child can no
longer reach any address in the parent's partition via near
pointers from its own segment.

This eliminates the entire "child stomps on parent's pages
post-execve" bug class.  No bookkeeping needed.

### 3.6 sys_brk

Becomes purely PCB-state:

```c
long sys_brk(uintptr_t addr) {
  if (addr == 0) return current->brk_current;
  uint16_t new_brk = (uint16_t)addr;
  if (new_brk < current->brk_base) return current->brk_current;
  if (new_brk >= current->mmap_low) return current->brk_current; /* OOM */
  current->brk_current = new_brk;
  return new_brk;
}
```

No allocation, no `mem_region_alloc_at`, no failure modes other
than colliding with the mmap region or stack.  Bytes inside the
segment between `brk_base` and `brk_current` are part of the
process's pre-allocated 64 KB partition; they exist whether or not
brk has been "extended" to them.

### 3.7 sys_mmap2 / sys_munmap

Becomes a per-process range tracker on the segment.  Track up to
N regions in PCB (e.g. 4-8 entries):

```c
typedef struct {
  uint16_t base;  /* segment-relative start */
  uint16_t size;  /* bytes; 0 = unused */
} i16_mmap_region_t;
```

`sys_mmap2` finds a free range high in the segment (between
`brk_current` and the user stack), records it, returns the
segment-relative address.  `sys_munmap` removes the entry.  No
allocation, no free, no global state.

`MAP_FIXED` is implementable: check that the requested range
fits and doesn't collide with existing regions or brk.

### 3.8 Idle thread (slot 0)

Slot 0 (the kernel idle thread) has **no user partition** — it
never enters user mode.  `i16_user_slot = 0xFF`.  It only uses its
kernel stack slot.

This means PROC_MAX = 3 gives **2 user slots**: enough for one
parent + one child, but not enough for shells with pipes or
backgrounded processes.  See §5 for the follow-up that releases
slot 0 to allow PROC_MAX = 3 + idle, giving 3 user slots.

---

## 4. Memory budget

| Region | Bytes | Notes |
|---|---|---|
| Core image (.text + .rodata + .data + .bss) at 0x0600..0x9FFF | ~37 KB | unchanged |
| VFS data 0xA000..0xD7FF | ~14 KB | unchanged (after the kstack parameterization currently in WIP) |
| Kernel stacks 0xD800..0xFBFF | 9 KB (3 × 3 KB) | unchanged |
| Core .text far CS=0x1000+ | ~34 KB | unchanged |
| VFS  .text far CS=0x1900+ | ~33 KB | unchanged |
| User partition 0 (PCB slot 1) at 0x22000 | 64 KB | **new reservation** |
| User partition 1 (PCB slot 2) at 0x32000 | 64 KB | **new reservation** |
| Kernel-side page pool above 0x42000 | ~372 KB | shrinks from 500 KB |
| BIOS / video at 0xA0000+ | n/a | unchanged |

Net cost: **128 KB** of conventional memory moved from the
general kernel page pool into static user-process partitions.
The kernel page pool is still 372 KB after the change, far more
than the kernel actually uses today.

---

## 5. Phasing

### Phase 1 — Static partition table (no behavioral change yet)

- Add `proc_layout.c` with the partition table.
- Add `i16_user_slot` to PCB.
- Add allocator for user slots in `proc_alloc` / `proc_free`.
- Reserve user partitions from the page pool at boot.
- Confirm pcxt still boots.  No semantic change yet.

### Phase 2 — execve uses pre-reserved partition

- `elf16_load_vnode` consults `current->i16_user_slot` and uses
  the partition's pages instead of `mem_region_page_alloc_contiguous`.
- Drop the `npages > 16` failure path.
- Remove `proc_track_page` calls for ELF pages on i16 — pages are
  permanently owned by the slot, not per-instance tracked.

### Phase 3 — sys_brk, sys_mmap2 simplification

- Convert `sys_brk` to PCB-only state.
- Implement `sys_mmap2` / `sys_munmap` as per-process range trackers.
- Remove i16-specific guards in `sys_mem.c` that exist only because
  the current allocator is segment-unaware.

### Phase 4 — Drop i16 page tracking for user pages

- `proc_track_page`, `proc_release_tracked_pages`,
  `proc_release_private_tracked_pages` become no-ops on i16
  (the partition stays allocated for the slot's lifetime).
- vfork's `proc_release_private_tracked_pages_from_array` no longer
  needed on i16 — execve doesn't free anything, just zeros and
  reloads.

### Phase 5 — Slot-0 release (separate proposal)

Release the kernel idle thread's PCB slot after kmain finishes
initialization, freeing slot 0 for a third user process.  Requires
scheduler refactor to tolerate `current == NULL` in the idle loop
or to fold idle behavior into pid 1 (init).  This is **not part of
this proposal** — it's listed as a follow-up so the eventual
benefit (3 user slots → pipes possible) is visible.

### Phase 6 — Possibly drop the pre-execve saved-frame dance

If a future redesign chooses **vfork-as-real-fork** on i16 (a 64 KB
memcpy at every vfork), the saved-frame mechanism in
`i16_vfork_restore_frame` and the `trap_ksp - 24` write in
`sys_vfork` can be removed entirely.  This is the cleanest
endpoint but costs ~10 ms per vfork on real PC/XT hardware.  It
is **out of scope** for this proposal — Phases 1-4 already give
post-execve isolation, which is the immediate goal.

---

## 6. Alternatives considered

### Alternative A — Keep variable-size allocation, fix the segment-aware allocator

Add a `proc_seg` parameter to `mem_region_page_alloc` and have it
filter for pages whose linear address is reachable from the given
segment.  Smaller diff, no memory cost, but:

- Doesn't fix vfork's shared-stack-class bugs.
- Doesn't simplify `sys_brk` / `sys_mmap2`.
- Adds a per-call constraint that other archs don't need.
- Still vulnerable to fragmentation: a process can't grow if no
  reachable page is free.

Rejected: smaller fix but doesn't address the underlying model
mismatch.

### Alternative B — vfork-as-real-fork

In addition to per-process partitions, change vfork to immediately
copy the parent's segment to the child's segment (`memcpy(child_base,
parent_base, 64KB)`), then patch the child's user-stack AX in its
own copy.  Eliminates the pre-execve shared-stack window entirely.

- Pro: Removes the saved-frame dance and `trap_ksp - 24` mechanism.
- Pro: Simplest mental model — vfork is just fork.
- Con: 64 KB memcpy per vfork.  ~10 ms on a real 4.77 MHz PC/XT,
  ~3 ms on a 10 MHz V30.  Acceptable but visible.

Tracked as Phase 6 / future work.  POSIX explicitly permits
implementing vfork as fork, so the userspace API doesn't change.

### Alternative C — Segment-aware top/bottom split with shared mmap pool

Keep variable-size allocation but partition the page pool such that
pages allocated for any single process come from a sub-pool whose
linear range is < (proc_seg << 4) + 64 KB.  Possible but complex —
need a sub-pool per active process and a different one per `proc_seg`.

Rejected: more complexity than just pre-reserving the segment.

---

## 7. Open questions

1. **Image-size assertion timing**: should the kernel reject any
   ELF whose `mem_end + ELF16_STACK_SIZE > 64 KB` at execve, or at
   build time via a CMake check on the user binary?  Currently
   `ELF16_MAX_SIZE = 60 KB` is a runtime check.  Build-time would
   give earlier feedback but requires adding a postlink size check
   to every pcxt user binary in the cmake list.

2. **Stack ceiling vs mmap floor**: where in the segment should
   the user stack live?  Today: top of allocation (`user_sp_top
   = alloc_size`).  Proposed: top of segment (`user_sp_top =
   0xFFFE`).  mmap allocates downward from just below the stack;
   brk grows upward from data end.  If they meet, OOM.  Need a
   small free space accounting helper.

3. **Cooperation with pcxt floppy DMA**: BIOS INT 13h reads to a
   16-bit linear address.  Today the floppy driver synthesizes
   this via `mem_region_page_linear(page) + off`.  Pages 0x22..
   0x31 (user partition 0) would have linear addresses up to
   0x31FFF — still fits in a 20-bit BIOS DMA address but not in
   16 bits.  The current driver assumes the buffer is in the SS=0
   segment (< 0x10000); user partition pages are above that, so
   any future direct user-buffer DMA would need to go through
   a kernel bounce buffer in DS=0.  Same constraint as today;
   not new.

4. **Slot-0 release dependency**: Phases 1-4 of this proposal can
   land without §5 (Slot-0 release), but the effective user-slot
   count is then 2, not 3.  Pipes / job control require 3+.  We
   should land this proposal first and §5 second.

---

## 8. Related documentation

- [PC Port Plan §3](pc_port.md) — i16 segment model, kernel layout.
- [Memory Management §10](../kernel/memory_management.md) — i16
  page-index discipline.
- [mem_region wrap-up](mem_region_wrapup.md) — Phase 1 commit
  history that exposed the bugs this proposal addresses.
- Commit `c113c49` — `pcxt: store vfork saved frame below trap_ksp,
  not above` — the immediate workaround that this proposal makes
  unnecessary in §6 / Alternative B.
