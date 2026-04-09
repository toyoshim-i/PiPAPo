# i16 Per-Process 64 KB Segment Allocation

> **Status**: In progress.  Phases 1 through 3 are now in place in the
> working tree; the remaining work is follow-up cleanup and later
> proposals such as reclaiming the skipped PCB slot.  Targets the
> kernel-stack overrun and shared-user-stack
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

1. **The user segment is implicit, not explicit.**  Today `execve`
   allocates only the minimum contiguous page run needed for
   `mem_end + ELF16_STACK_SIZE`, then derives `proc_seg` from the
   base of that allocation.  That *is* the real address-space
   identity of the process: every user-visible pointer is an offset
   inside that segment.  But the current model does not make the
   resulting 64 KB segment boundary explicit, so later heap / mmap
   growth can accidentally treat the process as if it lived in a
   flat global pool.

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

4. **`mmap` and late `brk` growth need a segment contract.**  On i16,
   user memory cannot be "any free page in the global pool"; it must
   stay inside the same 64 KB segment that `execve` established.
   Without that rule, the PPAP user stack of `mmap`-aware musl libc
   cannot run correctly on i16.

5. **Address arithmetic in `i16_vfork_restore_frame`** is non-trivial
   because it must compute `(user_ss * 16 + user_sp) - data_base` to
   find the user stack page within the parent's variable-size
   allocation.  If `image.data.base_page` is wrong (e.g. for a vfork
   child whose `image` was never copied from the parent), the math
   silently writes to the wrong page.

The cleanest structural fix is to make the segment model explicit:
on every i16 `execve`, the loader allocates **one contiguous 64 KB
window (16 × 4 KB pages)**, derives `proc_seg` from that allocation,
loads the ELF into pages 0..14, reserves page 15 as the user stack,
and keeps every later user allocation inside that same segment.

---

## 2. Goals

- **Make the i16 user segment explicit**: one contiguous 16-page
  allocation per exec'd process, with `proc_seg = base_linear >> 4`.
- **Eliminate the segment-unreachable allocation class** by ensuring
  every page a user process can ever own is inside that one 64 KB
  region addressable from its `proc_seg`.
- **Define a correct foundation for `sys_brk`, `sys_mmap2`, and
  `sys_munmap`** on i16: they must allocate or track only addresses
  inside the segment that `execve` already established.
- **Reduce the bug surface around vfork** by making post-execve
  segment isolation hardware-enforced (different exec-time segment →
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

### 3.1 Exec-time 64 KB user window

For i16, `execve` must allocate **exactly 16 contiguous pages** for the
new process image:

```c
page_id_t base_id = mem_region_page_alloc_contiguous(16u);
uint32_t base_linear = mem_region_page_linear(base_id);
uint16_t proc_seg = (uint16_t)(base_linear >> 4);
```

That allocation defines the whole user address space:

- pages `base_id + 0` .. `base_id + 14`: text + rodata + data + bss +
  future in-segment heap / mmap pages
- page `base_id + 15`: user stack page
- `CS = DS = ES = SS = proc_seg`
- every user pointer is a **16-bit segment-relative offset** inside
  `[0x0000, 0xFFFF]`

The important point is that the segment is identified by the
**allocated base page**, not by the PCB slot and not by a boot-time
reserved partition.

### 3.2 Page / `mem_region` interface contract

Phases 1 and 2 do **not** require a new arch-specific page allocator
API.  The necessary interface contract is:

1. Keep the generic page-pool API unchanged in
   [`page.h`](../../src/kernel/core/mm/page.h):
   - `page_alloc`
   - `page_free`
   - `page_alloc_contiguous`
   - `page_pool_base`

2. Keep the i16 loader and user-memory paths on the
   [`mem_region.h`](../../src/kernel/core/mm/mem_region.h) page-ID API:
   - `mem_region_page_alloc_contiguous(16)` allocates the user segment
   - `mem_region_page_linear(base_id)` derives `proc_seg`
   - `mem_region_page_read` / `mem_region_page_write` access the pages
     without requiring a near pointer
   - `mem_region_free` releases the owned segment on process teardown /
     failed `execve`

3. Do **not** treat `void *` returned by the raw page allocator as a
   stable model for i16 user pages above 64 KB.  i16 code should
   derive the segment from `page_id_t` and move bytes through
   `mem_region_page_*`.

4. Keep `user_pages[]` populated on i16.  In Phases 1 and 2 it remains
   the kernel's bookkeeping for which pages inside the 16-page segment
   belong to the process, so user-copy helpers and containment checks
   still work.

So the page-interface change is mostly a **policy change**:
i16 `execve` must use the page-ID-based contiguous allocator and must
derive the segment from that result.  It must not manufacture a
segment from a synthetic slot mapping.

### 3.3 `execve` / loader changes

`elf16_load_vnode` should keep the current detection / ELF-parse path,
but change the allocation model:

1. Compute the required loaded-image footprint from PT_LOAD headers.
2. Reject images whose text+data+bss footprint exceeds
   `15 * PAGE_SIZE` bytes.  The last page is always the user stack.
3. Allocate **exactly 16 contiguous pages** with
   `mem_region_page_alloc_contiguous(16)`.
4. Zero the entire 16-page window via `mem_region_page_write`.
5. Load every PT_LOAD segment into the allocated pages with
   `mem_region_page_write`.
6. Derive `proc_seg` from `mem_region_page_linear(base_id) >> 4`.
7. Set `image.text.base_page` / `image.data.base_page` from that real
   base page, and record logical occupancy in `user_pages[]` by segment
   page index.
8. Record `brk_base` / `brk_current` as the end of the loaded image,
   and reserve page 15 as non-heap stack space.

One important failure-path rule remains: successful `execve` must not
destroy the old image until the new one is fully loaded and its entry
frame is ready.  For same-process re-exec, the old 16-page segment must
therefore remain recoverable until loader I/O and stack setup succeed.

The resulting split is:

- `image.data` owns the whole 16-page segment and frees it on teardown
- `user_pages[]` records which segment pages are logically occupied by
  image / brk / stack / mmap

### 3.4 User stack = last page of the segment

The last page of the 16-page allocation is reserved for the user stack:

```text
base + 0x0000 .. base + 0xEFFF  loadable image + brk / mmap space
base + 0xF000 .. base + 0xFFFF  user stack page
```

The loader should:

- write argc/argv strings into page 15
- build the software frame and hardware `iret` frame in page 15
- set `exec_user_ss = proc_seg`
- set `exec_user_sp` to the segment-relative offset of the saved GP
  frame inside page 15

This makes the stack contract explicit instead of letting it float with
`alloc_size`.  It also gives `sys_mmap2` and `sys_brk` a stable ceiling:
they may grow upward only until they reach the reserved stack page.

### 3.5 vfork

The pre-execve window is unchanged: child shares the parent's
segment, kernel saves the parent's GP frame at `trap_ksp - 24`,
seeds the child's kernel stack with `[parent_user_sp,
parent_user_ss]`, patches `AX = 0` in the shared user stack frame.

The crucial change is **post-execve**: when the vfork child calls
`execve`, it gets its own fresh 16-page contiguous window.  The child's
`proc_seg` becomes different from the parent's because it is derived
from a different allocated base page, and after the next `iret` the
child runs with `CS = DS = SS = ES = child_proc_seg`.  The child can no
longer reach any address in the parent's segment via near pointers.

This eliminates the entire "child stomps on parent's pages
post-execve" bug class.  No bookkeeping needed.

### 3.6 `sys_brk`

The i16 rule for `sys_brk` is now:

- page 15 is never heap
- `brk` can only consume pages below page 15
- any newly exposed page must correspond to `base_id + n` for the same
  exec-time base page
- `brk` growth must fail if it would collide with an existing i16
  `mmap` range

This keeps `brk_base` / `brk_current` as segment-relative offsets while
the kernel updates low segment-page occupancy in `user_pages[]`.

### 3.7 `sys_mmap2` / `sys_munmap`

`sys_mmap2` / `sys_munmap` now become in-segment bookkeeping only:
no global search for a page outside the process segment.  The current
model keeps using `user_pages[]` as the occupancy map for i16, indexed
by the **logical page number inside the 16-page segment**:

```c
/* Conceptually:
 *   - slots 0..14 = image / brk / mmap occupancy for segment pages 0..14
 *   - slot 15     = user stack page
 * but every stored page ID is derived from the exec-time base page,
 * not from an unrelated global allocation.
 */
```

`sys_mmap2` finds a free range high in the segment (between
`brk_current` and the user stack), records the corresponding page IDs
in the matching logical slots, zeroes those pages for anonymous-mmap
semantics, and returns the segment-relative address.  `sys_munmap`
removes those occupancy entries without freeing the underlying segment,
because the process still owns the full 16-page window.

`MAP_FIXED` is implementable: check that the requested range
fits and doesn't collide with existing regions or brk.

### 3.8 Idle thread (slot 0)

Slot 0 (the kernel idle thread) never enters user mode, so it has no
16-page user segment.  It only uses its kernel stack slot.

---

## 4. Memory budget

| Region | Bytes | Notes |
|---|---|---|
| Core image (.text + .rodata + .data + .bss) at 0x0600..0x9FFF | ~37 KB | unchanged |
| VFS data at 0xA000..~0xBCE8 | ~7.4 KB | unchanged (matches current pcxt layout doc) |
| Kernel stacks 0xE000..0xFFFF | 8 KB (4 × 2 KB) | unchanged |
| Core .text far CS=0x1000+ | ~34 KB | unchanged |
| VFS  .text far CS=0x1900+ | ~33 KB | unchanged |
| Per-user exec allocation | 64 KB each | allocated on `execve`, not reserved at boot |
| Worst-case live user memory (`PROC_MAX = 4`) | up to 192 KB | only if all 3 user slots are active |
| Kernel-side page pool | unchanged at boot | shrinks only while user processes are actually resident |
| BIOS / video at 0xA0000+ | n/a | unchanged |

Net cost: no static reservation at boot.  The cost is **64 KB per live
user process**, allocated only when that process successfully execs a
native i16 image.

---

## 5. Phasing

### Phase 1 — Clarify and preserve the page-ID interface

Status: landed in commit `65fca01`.

- Do **not** add a new i16-only page allocator API.
- Keep `page.h` and `mem_region.h` interfaces generic.
- Ensure the loader path uses the existing page-ID-based contiguous
  allocation and `mem_region_page_*` accessors on i16.
- Confirm pcxt still boots with no semantic change beyond cleanup /
  invariants.

### Phase 2 — `execve` allocates one 16-page segment

Status: landed in commit `c23f122`.

- `elf16_load_vnode` allocates exactly 16 contiguous pages with
  `mem_region_page_alloc_contiguous(16)`.
- Derive `proc_seg` from that allocated base page.
- Load the ELF into pages 0..14 and reserve page 15 as the user stack.
- Establish the 16-page segment as one owned allocation; later phases
  refine `user_pages[]` from whole-segment tracking into logical
  occupancy bookkeeping.
- Add the minimal i16-only `sys_brk` path needed to expose bytes inside
  the already-owned segment without allocating fresh pages elsewhere.
- Preserve rollback semantics on loader failure.

### Phase 3 — sys_brk, sys_mmap2 simplification

Status: complete in the current design.

- Convert `sys_brk` to stay strictly inside the segment established by
  `execve`.
- Implement `sys_mmap2` / `sys_munmap` as in-segment tracking.
- Remove i16-specific behavior in `sys_mem.c` that assumes a flat pool.
- Keep `brk` / `mmap` occupancy in `user_pages[]` by logical segment
  page index.

### Phase 4 — Drop i16 page tracking for user pages

Status: largely folded into Phase 3.

Do **not** drop i16 page tracking outright.  Instead:

- Keep `user_pages[]` as the logical map of which pages inside the
  16-page segment are currently part of the loaded image, brk range,
  or mmap range.
- Separate **segment ownership** from **page occupancy bookkeeping**:
  the process still owns one real 16-page allocation that must be
  freed on process teardown, but helpers that manipulate `brk` / `mmap`
  ranges should only update metadata inside that segment.
- Keep the metadata path correct for user-copy helpers, procfs, ptrace
  containment checks, and `sys_mmap2` slot management.
- Rework i16-specific release helpers so they no longer free individual
  in-segment pages that are still part of the process's 16-page window.

### Phase 5 — Reclaim the skipped PCB slot (separate proposal)

Status: not started.

Remove the current temporary i16 kernel-stack workaround that makes
`proc_alloc()` skip one PCB slot.  Once that workaround is gone, the
existing `PROC_MAX = 4` configuration yields 3 schedulable user
processes (pid 0 remains the idle/kernel thread, plus 3 user slots).
This is **not part of this proposal** — it's listed as a follow-up so
the eventual benefit (3 user processes → pipes possible) is visible.

### Phase 6 — Possibly drop the pre-execve saved-frame dance

Status: not started.

If a future redesign chooses **vfork-as-real-fork** on i16 (a 64 KB
memcpy at every vfork), the saved-frame mechanism in
`i16_vfork_restore_frame` and the `trap_ksp - 24` write in
`sys_vfork` can be removed entirely.  This is the cleanest
endpoint but costs ~10 ms per vfork on real PC/XT hardware.  It
is **out of scope** for this proposal — Phases 1-4 already give
post-execve isolation, which is the immediate goal.

---

## 6. Alternatives considered

### Alternative A — Boot-time fixed partitions by PCB slot

Reserve 64 KB user partitions at boot and derive `proc_seg` from the
PCB slot rather than from the exec-time allocation.

- Pro: `sys_brk` / `sys_mmap2` bookkeeping becomes simpler later.
- Pro: no runtime fragmentation for user segments.
- Con: commits 64 KB per configured user slot up front.
- Con: ties the segment identity to PCB layout rather than the actual
  allocation that `execve` created.
- Con: makes same-slot `execve` rollback and the current skipped-PCB-slot
  workaround more awkward.

Rejected for now: it is a bigger policy change than needed for Phases
1-2, and it obscures the simpler rule that the user segment is defined
by the contiguous allocation returned by `execve`.

### Alternative B — vfork-as-real-fork

Change vfork to immediately allocate a fresh 16-page segment for the
child, copy the parent's segment into it (`memcpy(child_base,
parent_base, 64 KB)`), then patch the child's user-stack AX in its own
copy.  Eliminates the pre-execve shared-stack window entirely.

- Pro: Removes the saved-frame dance and `trap_ksp - 24` mechanism.
- Pro: Simplest mental model — vfork is just fork.
- Con: 64 KB memcpy per vfork.  ~10 ms on a real 4.77 MHz PC/XT,
  ~3 ms on a 10 MHz V30.  Acceptable but visible.

Tracked as Phase 6 / future work.  POSIX explicitly permits
implementing vfork as fork, so the userspace API doesn't change.

### Alternative C — Variable-size exec allocation with a separate stack page

Keep variable-size exec allocation, but continue placing the stack at
the top of the loaded image and allocate an extra stack page only when
needed.

- Pro: smaller resident footprint for tiny binaries.
- Con: keeps the stack contract implicit and moving with `alloc_size`.
- Con: pushes `brk` / `mmap` back into "which pages are reachable from
  this segment?" checks.
- Con: reintroduces per-call segment-reachability policy instead of
  one fixed 16-page invariant.

Rejected: saves memory, but keeps the exact model ambiguity that caused
the current confusion.

---

## 7. Open questions

1. **Image-size assertion timing**: should the kernel reject any
   ELF whose `mem_end + ELF16_STACK_SIZE > 64 KB` at execve, or at
   build time via a CMake check on the user binary?  Currently
   `ELF16_MAX_SIZE = 60 KB` is a runtime check.  Build-time would
   give earlier feedback but requires adding a postlink size check
   to every pcxt user binary in the cmake list.

2. **Stack-page exact top-of-stack**: should the initial user SP start
   at the very top of page 15 (`0xFFFE`) or leave a small red zone /
   guard margin below it for argc/argv and the saved frames?  The
   current proposal fixes the stack *page* but still leaves the exact
   first-SP convention to the loader implementation.

3. **Cooperation with pcxt floppy DMA**: BIOS INT 13h reads to a
   16-bit linear address.  Today the floppy driver synthesizes
   this via `mem_region_page_linear(page) + off`.  Pages 0x22..
   0x31 for a typical first user segment would have linear addresses
   above `0xFFFF` — still fits in a 20-bit BIOS DMA address but not in
   16 bits.  The current driver assumes the buffer is in the SS=0
   segment (< 0x10000); user pages are above that, so
   any future direct user-buffer DMA would need to go through
   a kernel bounce buffer in DS=0.  Same constraint as today;
   not new.

4. **Skipped-PCB-slot dependency**: Phases 1-4 of this proposal can
   land without §5, but the effective schedulable user-process count
   on current pcxt remains 2 until the temporary i16 kernel-stack
   workaround is removed.  Pipes / job control require 3+.  We should
   land this proposal first and the kstack follow-up second.

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
