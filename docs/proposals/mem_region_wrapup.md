## mem_region Wrap-Up: Deprecating `mem_helper.h` and Closing Issue #48

> **Status**: Phases 1–4 complete. `mem_helper.h` deleted, single-page
> contract enforced, bounce buffers removed. Phase 5 (core loader
> migration) is deferred to a separate proposal.

### 1. Background

PPAP's memory model uses `(page_id_t page, uint16_t off)` tuples instead of
raw `void *` for any buffer that may live in user-process pages. This
discipline exists for one hard reason: on i16 (pcxt), `void *` is 16-bit
and cannot reach pages above 64 KB. The same discipline also benefits
32-bit targets by removing implicit assumptions about flat addressability,
and it sets up a clean MMU story for future targets.

Two specifications already define how this should look end-to-end:

- [Memory Management §3 / §9](../kernel/memory_management.md) — defines the
  page-index API (`mem_region_page_alloc/free/read/write/linear`,
  `mem_region_ptr_to_page`).
- [PC Port Plan §7](pc_port.md) — defines the syscall-boundary conversion
  (`user_to_page`) and the read/write data path
  (`fd_read/fd_write` accept `(desc, page, off, n)`).

In practice, the VFS read/write data path **already** carries `(page, off)`
end-to-end through `vfs_ops_t.read/write` and the file-system drivers
(`ufs`, `vfat`, `tmpfs`, `romfs`, `procfs`, `devfs`). The cursor helpers in
`mem_helper.h` (`mem_region_page_chunk_len`, `mem_region_page_advance`)
implement exactly the cursor walk that this discipline requires.

What is *not* clean today:

1. The cursor helpers live under `kernel/common/subtle/` with a
   "legacy, do not add new uses" warning. Phase 1 of this proposal
   makes them genuinely unnecessary by promoting "single page per
   leaf call" to a universal contract — after Phase 1, no leaf walks
   a cursor, and the helpers can be deleted with no replacement.
2. `mem_helper.h` also exposes `mem_region_ptr_ref()`, a `void *` →
   `(page, off)` encoder used in nine straggler call sites that predate
   the §7 discipline. Each call site is a place where a `void *` enters or
   leaves the page-indexed world without going through `user_to_page`.
3. `mem_region_page_linear()` is documented in `mod_core` as a "subtle
   workaround for blkdev DMA"
   ([`mod_core.inc:21`](../../src/kernel/common/mod/mod_core.inc),
   [`mod_core.h:136`](../../src/kernel/common/mod/mod_core.h)). The
   "subtle/blkdev-only" framing is wrong: an audit shows the function
   has many *legitimate* callers in core for pure 32-bit arithmetic
   (brk/munmap address matching, signal/exec stack-top computation,
   subsys address reporting) — exactly the uses sanctioned by
   [memory_management.md §9](../kernel/memory_management.md). Issue #48
   is really about removing the **`void *` buffer** from `blkdev_t`,
   not removing `mem_region_page_linear` itself. After this proposal
   the entry stays in `mod_core` but its docstring is rewritten to
   match §9, and the only true R3 violation —
   [`vfs/devfs.c`](../../src/kernel/vfs/devfs.c) fabricating
   `(void *)(linear + off)` to call nested driver callbacks — is
   fixed in Phase 1.
4. Several straggler call sites use a kernel-stack **bounce buffer** that
   adds an avoidable `memcpy` and (on i16) eats scarce kernel data segment
   space.

This proposal enumerates the design rules, the call-site survey, and a
phased plan to land all four cleanups together.

---

### 2. Priorities

The phases below are not equal-weight. From highest to lowest:

1. **Phase 1 — `blkdev` page-indexed signature.** This is the priority.
   It aligns the storage I/O path with the page/offset discipline that
   already governs the VFS read/write path. Issue #48 has been pending
   since the blkdev relocation in commit `42a84ec`. Phase 1 also fixes
   the one true R3 violation in [`vfs/devfs.c`](../../src/kernel/vfs/devfs.c),
   which casts `linear + off` to `void *` to call nested driver
   callbacks. **`mem_region_page_linear` itself stays in `mod_core`**;
   its many legitimate arithmetic callers in `core/` are sanctioned by
   [memory_management.md §9](../kernel/memory_management.md) and are
   not in scope.

2. **Phase 2 — Single-page contract, cursor helper removal.**
   Gated on Phase 1. Enforces single-page I/O at the syscall
   boundary (page-walk loop in `sys_io.c`) and removes
   `page_chunk_len` / `page_advance` from all VFS leaves.
   `mem_helper.h` is stripped to `ptr_ref` only — four bridge
   callers still need it.

3. **Phase 3 — Bridge conversion, `mem_helper.h` deletion.**
   Converts bridge signatures from `void *` to `(page, off)`,
   eliminating the last `ptr_ref` callers. Deletes `mem_helper.h`
   and `subtle/`. Also removes per-call bounce buffers — a
   measurable performance + footprint win on m68k/x68k.

4. **Phase 4 — Documentation and lint.** Mechanical follow-up.

**Deprioritized — not part of this proposal:** core-side loader changes
in [`core/exec/exec.c`](../../src/kernel/core/exec/exec.c) and
[`core/exec/elf16_loader.c`](../../src/kernel/core/exec/elf16_loader.c).
These have `ptr_ref` call sites but they are tangled with process
lifecycle (`proc_alloc`, `proc_setup_stack`, `proc_track_page_range`,
`image_release_owned_segments`) that lives entirely on the core side
and would need its own design pass. Touching them in this proposal
would expand scope beyond the page/offset alignment that motivates the
work. The two `ptr_ref` sites in `exec.c` and the one in
`elf16_loader.c` will be left with the encoding inlined locally and a
`TODO(mem_region_wrapup Phase 5)` comment pointing back at this
document. They are tracked as Phase 5 for a future proposal.

---

### 3. Design Rules

These rules govern every change in the proposal. They are not new — most
already appear in the kernel module / memory management docs — but they
are restated here as a single checklist so each phase can be evaluated
against them.

#### R1. Page-indexed I/O end-to-end

Inside `src/kernel/vfs/`, `src/kernel/core/exec/`,
`src/kernel/core/subsys/`, and `src/kernel/syscall/`, file and user
buffers are carried as `(page_id_t, uint16_t)` pairs. `void *` may exist
only above the syscall dispatcher (where it came from userspace) or
strictly inside hardware drivers (where it represents a physical address
about to be programmed into a DMA register or BIOS call).

#### R2. Conversions are tightly scoped

There is exactly **one** sanctioned general-purpose `void *` → `(page, off)` conversion
in the kernel for user-space arguments: **`user_to_page(base_page, user_off)`** at the syscall
dispatcher ([pc_port.md §7](pc_port.md)). It is per-architecture and lives next to
the syscall entry path.

For **kernel-owned buffers only**, a tightly scoped inline helper (e.g., `mem_region_kbuf_to_page`)
may be placed in `kernel/common/`. This is strictly reserved for legitimate internal
metadata I/O (e.g., `ufs_buf` or `sector_buf` in file systems) and boot-time parsing
under R4 constraints.

All other apparent need to "convert a pointer to (page, off)" is a
**design smell** indicating one of:

- a bounce buffer that should not exist (R4 violation), or
- a function that should be receiving `(page, off)` directly from its
  caller, not a `void *`, or
- a driver that needs a hardware linear address (R3 escape hatch — but
  the conversion is the driver's *internal* business and uses
  `mem_region_page_linear()` directly, never a generic helper).

The previous `mem_region_ptr_ref()` / `mem_helper.h` was overly general and
let call sites avoid asking the harder design questions. Replacing it with an explicit
kbuf-only inline helper *forces* those questions to be answered for non-metadata I/O.

#### R3. `mem_region_page_linear()` returns `uint32_t`, never `void *`

`mem_region_page_linear()` is the sanctioned API for obtaining the
32-bit linear address of a page (see
[memory_management.md §9](../kernel/memory_management.md)). It may
be used freely for:

- Address arithmetic and offsets (e.g. `brk` growth, stack-top
  computation).
- Address-range containment checks (e.g. `proc_page_backed_contains`,
  `sys_munmap` lookup).
- Reporting addresses to userspace or subsystem bridges.
- Programming hardware (BIOS, DMA, memory-mapped registers) inside
  drivers.

What R3 forbids is the **`(void *)(mem_region_page_linear(id) + off)`
cast pattern**: synthesising a `void *` from a linear address to feed
some other API. On i16 the cast truncates above 64 KB and silently
corrupts; on 32-bit it works but bypasses the page-indexed I/O
discipline. The fix is always to pass `(page, off)` to the callee
instead of fabricating a pointer.

The only current offender in non-driver code is
[`vfs/devfs.c`](../../src/kernel/vfs/devfs.c), where node read/write
fabricate a `void *` to call nested driver callbacks. Phase 1 fixes
this by changing the devfs-internal driver callback signature to take
`(page, off)`.

#### R4. Avoid bounce buffers

Do not introduce a kernel-stack or kernel-heap intermediate buffer just
to bridge between two `(page, off)` consumers. Pass `(page, off)` from
the producer to the eventual `mem_region_page_read/write` site so the
data lands directly in its destination. This matters for two reasons:

- **Performance** — the extra `memcpy` is wasted on every read/write.
- **i16 footprint** — the kernel data segment on pcxt is bounded by the
  64 KB DS=0 split (see [kernel_modules.md §i16 Segment Split](../kernel/kernel_modules.md))
  and core .text + .rodata + .data + .bss already consume most of
  0x0600–0x9FFF (~38 KB) per [pc_port.md §12](pc_port.md). Every
  unnecessary stack buffer eats into this budget.

Bounce buffers are permitted only when:

- The data is parsed in-place and discarded (e.g. boot-time config text
  like `/etc/fstab`, where the parsed result is a fixed `fstab_entry_t[]`
  array, not the raw text), **and**
- The buffer is small (≤ 512 B), **and**
- The kbuf lifetime is confined to a single function.

In every other case, allocate the destination once and pass `(page, off)`
to the producer.

#### R5. No new `mod_core` / `mod_vfs` entries

The module surface is bounded:

- Each `mod_*` entry costs a struct slot, an `.inc` line, and on i16 a
  pair of two-level far-call stubs in `*_stubs.S` / `*_entries.S`.
- The pcxt code budget is already tight per
  [pc_port.md §12 Size Constraint](pc_port.md).

Any helper that two modules need *and* is pure inline arithmetic /
local pointer math goes into `kernel/common/` as a `static inline`. Any
helper that requires a real cross-module call must justify itself
against the alternative of **moving the file to the right module**
(see [Future Work in kernel_modules.md](../kernel/kernel_modules.md)).

This proposal keeps the `mod_core` surface unchanged (R3 was
relaxed during the Phase 1 audit — see §1) and adds zero new
entries.

#### R6. Module-relocation over interface growth

If two pieces of code need to communicate across a module boundary,
prefer moving one of them to the other's module over adding a `mod_*`
entry. Drivers in particular: a driver that backs a vnode or a
filesystem object belongs in `vfs/driver/`, not `core/driver/`. A
driver that is purely a service to mod_core (klog backend, page-pool
allocator backend) may stay in core.

#### R7. Page-walk responsibility lives in core

The VFS module never advances a `(page, off)` cursor across a page
boundary. When a syscall receives a user buffer that spans multiple
pages, the **core syscall dispatcher** (`sys_io.c`) splits it into
per-page chunks and issues one `mod_vfs.fd_read/fd_write` call per
chunk. VFS leaves may assume `off + n ≤ PAGE_SIZE` (single-page
contract).

Rationale:

- **Simplicity in VFS:** file-system drivers handle at most one page's
  worth of data per call. They never need `mem_region_page_advance` or
  `mem_region_page_chunk_len` — those helpers become unnecessary.
- **Module boundary clarity:** page-index knowledge (how to find the
  next page given a page ID) stays in core where `mem_region` lives.
  VFS does not need to know how pages are numbered or laid out.
- **Acceptable cost:** the far-call overhead per iteration on i16 is
  small compared to the actual I/O work (sector reads, cluster walks,
  etc.).

Other root callers (eCPU bridges, `sys_writev`/`sys_readv`) follow the
same pattern: split by page in core before entering VFS.

---

### 3. Call-Site Survey

`mem_helper.h` provides three helpers. Their callers, classified:

#### 3.1 Cursor math (`page_chunk_len`, `page_advance`) — 14 sites, soon obsolete

| File | Sites | Notes |
|---|---|---|
| [`vfs/romfs.c`](../../src/kernel/vfs/romfs.c) | 2 | inside `romfs_read` |
| [`vfs/ufs.c`](../../src/kernel/vfs/ufs.c) | 2 | inside `ufs_read`, `ufs_write` |
| [`vfs/procfs.c`](../../src/kernel/vfs/procfs.c) | 2 | inside `procfs_read` |
| [`vfs/devfs.c`](../../src/kernel/vfs/devfs.c) | ~14 | every devfs node read/write |
| [`vfs/vfat.c`](../../src/kernel/vfs/vfat.c) | 2 | inside `vfat_read`, `vfat_write` |
| [`vfs/tmpfs.c`](../../src/kernel/vfs/tmpfs.c) | 2 | inside `tmpfs_read`, `tmpfs_write` |

These currently take `(page, off, n)` from `vfs_ops.read/write`, walk
the cursor inside the leaf with `page_chunk_len` / `page_advance`, and
store data via `mem_region_page_read/write`. **No bounce buffers.**

Under the Phase 1 single-page contract (see step 1.1), every leaf
becomes single-page: it handles at most one chunk that does not cross
a page boundary and returns the bytes actually handled. The cursor
walk **moves out of the leaves and up to the root caller** (the
syscall dispatcher in `sys_io.c` / eCPU bridge). After
Phase 1 lands, all 14 sites in this table simplify: the inner cursor
loop and the calls to `page_chunk_len` / `page_advance` are deleted
together with the surrounding `while (remaining)`. The root caller
drives the loop instead.

This is the reason Phase 2 (`mem_helper.h` deletion) needs no
replacement helper file at all: by the time it lands, the cursor
helpers have no callers.

#### 3.2 `ptr_ref` callers — 9 sites, four dispositions

| File | Site | Disposition |
|---|---|---|
| [`vfs/driver/loopback.c:55,77`](../../src/kernel/vfs/driver/loopback.c) | `loop_read/write` | **Phase 1** — falls out for free once `blkdev_t.read/write` takes `(page, off)` |
| [`core/subsys/human68k_bridge.c:42`](../../src/kernel/core/subsys/human68k_bridge.c) | `dos_read_buf` | **Phase 3** — bridge helper signature changes to take `(page, off)` from the eCPU memory model; bounce removed |
| [`core/subsys/sos_bridge.c:38`](../../src/kernel/core/subsys/sos_bridge.c) | similar | **Phase 3** |
| [`core/subsys/cpm_bridge.c:32`](../../src/kernel/core/subsys/cpm_bridge.c) | similar | **Phase 3** |
| [`core/exec/h68k_emu.c:43`](../../src/kernel/core/exec/h68k_emu.c) | emulator memory shim | **Phase 3** — caller is the bridges, fix follows from their fix |
| [`vfs/fstab.c:88`](../../src/kernel/vfs/fstab.c) | `fstab_parse` | **Phase 2** — legitimate kbuf under R4; encoding goes inline with a comment, no shared helper |
| [`core/exec/exec.c:38,111`](../../src/kernel/core/exec/exec.c) | argv staging, ELF segment load | **Deferred (Phase 5)** — entangled with process lifecycle, out of scope |
| [`core/exec/elf16_loader.c:65`](../../src/kernel/core/exec/elf16_loader.c) | ELF header read | **Deferred (Phase 5)** |

The deferred sites continue to call an inline encoding (same body as
the old `ptr_ref`) until Phase 5 lands as a separate proposal. The
encoding is **not** exposed as a shared symbol — each deferred site
inlines it locally with a `TODO(mem_region_wrapup Phase 5)` comment.
That keeps R2 honest: there is no public temptation, the only
remaining offenders are clearly marked.

#### 3.3 `mem_region_page_linear` callers — audit results

Audited 2026-04-07. After R3 was relaxed (see §1 / §3 R3), the only
true violation is the `(void *)(linear + off)` cast pattern. The
audit found exactly one offender outside drivers:

| File | Sites | Disposition |
|---|---|---|
| [`vfs/devfs.c:303,330,365,395`](../../src/kernel/vfs/devfs.c) | 4 | **Phase 1** — devfs driver callback signature changes to `(page, off)`; cast removed |

All other non-driver callers (`signal.c`, `sys_proc.c`, `sys_mem.c`,
`proc.c`, `elf16_loader.c`, `elf_loader.c`, `human68k_bridge.c`,
`i16_common.c`) use the function for legitimate `uint32_t`
arithmetic per [memory_management.md §9](../kernel/memory_management.md)
and remain unchanged.

---

### 4. Phased Plan

Phases land in numeric order **1 → 2 → 3 → 4**, with Phase 5 deferred to a
later proposal. Each leaves the tree building and tested on qemu_arm +
qemu_m68k + pcxt at minimum; floppy-using phases also test x68k.

#### Phase 1 — Close issue #48 (`blkdev` page-indexed signature) — **COMPLETE**

> All steps verified 2026-04-11 against the codebase:
> - 1.0: `mem_region_kbuf_to_page` exists in
>   [`kernel/common/mem_region_kbuf.h`](../../src/kernel/common/mem_region_kbuf.h).
> - 1.1: `blkdev_t.read/write` takes `(page_id_t page, uint16_t off)` in
>   [`vfs/driver/blkdev.h`](../../src/kernel/vfs/driver/blkdev.h).
> - 1.2: Confirmed — `mod_core.inc` has 18 entries, none for blkdev.
> - 1.3: loopback and pcxt floppy drivers use the page-indexed signature.
> - 1.4: `ufs.c` and `vfat.c` use `mem_region_kbuf_to_page` for metadata I/O.
> - 1.5: devfs callbacks already take `(page_id_t, uint16_t off)`;
>   the `(void *)(linear + off)` R3 cast is gone.
> - 1.6: `mod_core.h` docstring says "Sanctioned API for arithmetic and
>   address-range checks", no longer "subtle workaround".

1.0. **Add `mem_region_kbuf_to_page` inline helper** in
    `kernel/common/` (R2 kbuf escape hatch). This is a strictly
    scoped `static inline` that converts a kernel-owned buffer
    pointer (e.g. `ufs_buf`, `sector_buf`) to a `(page, off)` pair via
    `mem_region_ptr_to_page`. It exists so file-system drivers can
    call the new page-indexed `blkdev` API for metadata I/O without
    reaching for `ptr_ref`. Allowed callers are documented in the
    helper's header comment.

1.1. Change `blkdev_t.read` and `blkdev_t.write` signatures from
    `(struct blkdev *, void *buf, uint32_t sector, uint32_t count)` to
    `(struct blkdev *, page_id_t page, uint16_t off, uint32_t sector,
    uint32_t count)`.

    **Page-boundary contract.** A single `blkdev_t.read/write` call
    must not span more than one page. The callee assumes
    `off + count*BLKDEV_SECTOR_SIZE ≤ PAGE_SIZE` and handles at most
    `(PAGE_SIZE - off) / BLKDEV_SECTOR_SIZE` sectors per invocation.
    The return value is the number of sectors actually handled (≥ 0)
    or a negative errno. The callee never advances `page` itself —
    crossing a page boundary is the *caller's* responsibility.

    In Phase 1 the single-page contract applies **only** to the new
    `blkdev_t.read/write`. `vfs_ops_t.read/write` keeps its existing
    multi-page interface and the §3.1 cursor walks remain in place.
    Converting `vfs_ops` to single-page (and the corresponding page
    walk in `fd.c vfs_bridge_read/write`) is the load-bearing work of
    Phase 2 — it's what eliminates the §3.1 cursor-helper callers and
    lets `mem_helper.h` be deleted.

    The Phase 1 **root caller** for blkdev is
    [`vfs/devfs.c`](../../src/kernel/vfs/devfs.c) (devblk_read/write,
    devloop_read_n/write_n) — it already walks a (page, off) cursor;
    after Phase 1 it just stops casting to `void *`. For metadata I/O
    in `ufs.c`/`vfat.c`/`fstab.c`, the buffer is a single static
    kernel buffer (`ufs_buf`, `sector_buf`) with `count=1`, so the
    single-page contract is satisfied trivially via
    `mem_region_kbuf_to_page`. It computes
    `chunk = min(remaining, PAGE_SIZE - off)`, issues the call,
    advances `(page, off)` by the returned count, and repeats until
    `remaining == 0` or a short read terminates it. This is the same
    cursor walk that file-system drivers in §3.1 already perform via
    `page_chunk_len` / `page_advance`; Phase 1 makes it the universal
    contract instead of a VFS-internal convention.

    Rationale:

    - Keeps every leaf simple: no `(page, off)` arithmetic, no
      cross-page memcpy, no allocation, no failure mode for "tried to
      read across an unmapped page".
    - On i16, single-page semantics let the leaf set the segment
      register exactly once and stay there for the whole call.
    - Matches POSIX `read(2)` short-read semantics, so the loop
      structure that the dispatcher already implements for short reads
      handles page-boundary partials with no new code path.
    - Removes any temptation for a leaf to reach for
      `mem_region_page_linear` "just to compute the next page" — leaves
      never see the next page.

    The contract is documented in
    [`vfs_types.h`](../../src/kernel/common/vfs/vfs_types.h) (above
    `vfs_ops_t`), in the new `blkdev_t` definition, and in the Phase 4
    invariant block in `memory_management.md`.

1.2. **No `mod_core` change.** Audited 2026-04-07: `mod_core.inc` has
    no `blkdev_read` / `blkdev_write` entries. blkdev is a VFS-internal
    interface — all backends and callers live in the VFS module
    (including pcxt floppy_blk in `target/pcxt/kernel/vfs/driver/`),
    so `dev->read/write` is a same-module function-pointer call with
    no stub marshaling. The earlier
    [`kernel_modules.md`](../kernel/kernel_modules.md) docs mentioning
    `mod_core.blkdev_read/write` are stale and will be refreshed in
    Phase 4.

1.3. Update every `blkdev_t` implementation:

| Driver | Current location | After 1.3 |
|---|---|---|
| `loopback` | `vfs/driver/loopback.c` | forwards `(page, off)` to backing vnode op; **`ptr_ref` removed** |
| pcxt floppy | `kernel/vfs/driver/` (moved to VFS module per commit `42a84ec`) | calls `mem_region_page_linear(page) + off` once, in the BIOS INT 13h call site, with `/* §R3 hardware escape hatch */` |
| x68k floppy | `kernel/vfs/driver/` | same pattern at the IOCS call |
| RP2040 SD/SPI block | `kernel/{core,vfs}/driver/` (TBD by R6) | same pattern at the DMA register write |
| Any other backend | — | inventory and convert |

1.4. Update every blkdev *caller* (the file-system drivers that read
    sectors via `mod_core.blkdev_read/write`). For user payload I/O, they
    already hold `(page, off)` from VFS above, so this is mechanical.
    For metadata I/O (e.g., superblocks, FAT tables, directory entries
    read into static buffers like `ufs_buf` or `sector_buf`), they will use
    the new `mem_region_kbuf_to_page` inline helper introduced in
    `kernel/common/` to pass their kernel buffers to the `blkdev` API.

1.5. **Fix [`vfs/devfs.c`](../../src/kernel/vfs/devfs.c) — the only true
    R3 violation.** The four call sites at lines 303/330/365/395 cast
    `(void *)(mem_region_page_linear(page) + off)` to call nested
    devfs node callbacks. Change the devfs-internal callback type to
    `(page_id_t, uint16_t off, ..., size_t n)` and pass the pair
    through unchanged. All four call sites lose the cast.

1.6. **Update `mod_core` documentation** in
    [`mod_core.h`](../../src/kernel/common/mod/mod_core.h) and
    [`memory_management.md §3.4`](../kernel/memory_management.md): the
    `mem_region_page_linear` entry is *not* a "subtle workaround". It
    is the sanctioned API per §9 for arithmetic and address-range
    checks. Phase 4 mechanically refreshes the rest of the docs; this
    step just removes the apologetic footnote so the contract is
    visible during the rest of Phase 1's work. **No vtable change**:
    indices in `mod_core.inc` are untouched, no stub regeneration, no
    `PATCH_CORE` renumbering.

#### Phase 2 — Enforce single-page contract, remove cursor helpers from VFS

Goal: eliminate the §3.1 cursor-helper callers (`page_chunk_len`,
`page_advance`) by enforcing the single-page contract at the syscall
boundary. The page-walk loop lives in the **core syscall dispatcher**
([`sys_io.c`](../../src/kernel/core/syscall/sys_io.c)), not in VFS.
This means VFS never needs to know how to find the next page — it
just handles what fits in one page and returns.

After Phase 2, `mem_helper.h` is stripped to `ptr_ref` only.
The file is deleted in Phase 3 after the bridge callers are converted.

Prerequisite: Phase 1 has landed (blkdev page-indexed signature). ✓

2.0. **Add page-walk loop to `sys_read`/`sys_write` in
    [`sys_io.c`](../../src/kernel/core/syscall/sys_io.c).**
    The syscall dispatcher already resolves the user pointer to a
    `user_page_ref_t` via `proc_user_ptr_to_page_ref`. Add a loop
    around the existing `mod_vfs.fd_read/fd_write` call that issues
    one VFS call per page chunk and advances the ref between calls
    using the existing `sys_io_advance_ref` helper:

```c
size_t total = 0;
while (total < n) {
  size_t remaining = n - total;
  uint16_t chunk = (PAGE_SIZE - ref.off < remaining)
                 ? (uint16_t)(PAGE_SIZE - ref.off) : (uint16_t)remaining;
  long ret = mod_vfs.fd_read((int)desc, ref.page, ref.off, chunk);
  if (ret <= 0) return total > 0 ? (long)total : ret;
  total += (size_t)ret;
  if ((size_t)ret < chunk) break; /* short read */
  sys_io_advance_ref(&ref, (size_t)ret);
}
return (long)total;
```

    **Why core, not VFS:** the far-call cost per iteration is small
    compared to the actual I/O. Keeping the loop in core means VFS
    never needs page-advance arithmetic — it just handles what fits
    in one page and returns. This also keeps the page-index knowledge
    (how to find the next page) out of the VFS module entirely.

    Short-read semantics: if the leaf returns fewer bytes than
    `chunk` (e.g. EOF mid-page), the loop breaks. This matches the
    existing POSIX short-read contract.

    **Other callers:** `sys_writev`/`sys_readv` delegate to
    `sys_write`/`sys_read`, so they inherit the page walk
    automatically. The eCPU bridge callers (Phase 3) will need
    their own page-walk loops when their signatures change.

2.1. **Convert each FS leaf to single-page semantics.**
    The caller (step 2.0) guarantees `off + n ≤ PAGE_SIZE`. Each
    leaf handles at most `n` bytes within a single page and returns
    the number actually handled. Per-leaf changes:

    **Simple leaves** (flat source data, cursor loop removed entirely):

    - [`tmpfs.c`](../../src/kernel/vfs/tmpfs.c) `tmpfs_read/write`:
      Remove the `while (remaining)` loop and the
      `chunk_len`/`advance` calls. Single
      `mem_region_page_write(page, off, src, n)` for read,
      `mem_region_page_read(page, off, dst, n)` for write. The
      zero-fill inner loop in `tmpfs_read` (for sparse files) stays
      but operates on a single chunk.
    - [`romfs.c`](../../src/kernel/vfs/romfs.c) `romfs_read`: Remove
      cursor loop. Single `page_write(page, off, src, n)`.
    - [`procfs.c`](../../src/kernel/vfs/procfs.c) `procfs_read`:
      Remove cursor loop. Single `page_write(page, off, src, n)`.

    **Block-structured leaves** (sector/cluster loop stays, page
    cursor arithmetic simplified):

    - [`ufs.c`](../../src/kernel/vfs/ufs.c) `ufs_read/write`: The
      sector-walk loop is file-structure-driven (block map → sector
      read → partial-sector copy) and stays. Under the single-page
      contract, the memory cursor never crosses a page boundary, so
      `mem_region_page_advance(&page, &page_off, avail)` becomes
      `page_off += (uint16_t)avail` and
      `mem_region_page_chunk_len(page_off, remaining)` is unnecessary
      (`remaining` already fits within the page).
    - [`vfat.c`](../../src/kernel/vfs/vfat.c) `vfat_read/write`:
      Same treatment as ufs — cluster-chain and sector loops stay,
      `page_advance` → `page_off +=`, `chunk_len` dropped.

2.2. **Inline devfs cursor arithmetic.**
    [`devfs.c`](../../src/kernel/vfs/devfs.c): `devblk_read/write`
    and `devloop_read_n/write_n` are the raw block-device paths.
    They walk sectors with a page cursor. Under the single-page
    contract from the devfs `read/write` entry points, replace
    `mem_region_page_advance` with `page_off += chunk` and drop
    `mem_region_page_chunk_len`.

    After this step, `mem_region_page_chunk_len` and
    `mem_region_page_advance` have **zero callers**.

2.3. **Replace `ptr_ref` callers in VFS.**
    `mem_region_kbuf_to_page` already exists from Phase 1
    ([`kernel/common/mem_region_kbuf.h`](../../src/kernel/common/mem_region_kbuf.h)).

    | File | Action |
    |---|---|
    | [`vfs/fstab.c:88`](../../src/kernel/vfs/fstab.c) | Switch from `mem_region_ptr_ref` to `mem_region_kbuf_to_page` (legitimate kbuf under R4). |

2.4. **Replace `ptr_ref` callers in core loaders.**

    | File | Action |
    |---|---|
    | [`core/exec/exec.c:38,111`](../../src/kernel/core/exec/exec.c) | Inline the `ptr_ref` encoding locally with a `TODO(mem_region_wrapup Phase 5)` comment. Deferred — entangled with process lifecycle. |
    | [`core/exec/elf16_loader.c:65`](../../src/kernel/core/exec/elf16_loader.c) | Same — inline encoding + TODO. Deferred. |

2.5. **Strip `mem_helper.h` to `ptr_ref` only.**
    Remove `page_chunk_len` and `page_advance` (zero callers after
    steps 2.1–2.2). Remove the `#include` from all VFS files.
    `mem_region.h` retains the include for the four bridge callers
    (`human68k_bridge.c`, `sos_bridge.c`, `cpm_bridge.c`,
    `h68k_emu.c`) that still use `ptr_ref`.

2.6. **Update docs.**
    - [`memory_management.md §3.2`](../kernel/memory_management.md):
      update `mem_helper.h` framing to "pending deletion".
    - [`kernel_modules.md` directory listing](../kernel/kernel_modules.md):
      correct mod_core function count from 20 to 18.

After Phase 2, no `page_chunk_len`, no `page_advance`, no cursor
walks in VFS leaves. `mem_helper.h` persists with `ptr_ref` only
for the four bridge callers. Phase 3 converts the bridge signatures
to `(page, off)`, eliminating the last `ptr_ref` callers, and then
deletes `mem_helper.h` and `subtle/`.

#### Phase 3 — Bridge conversion, bounce-buffer elimination, delete `mem_helper.h`

Sites: [`human68k_bridge.c`](../../src/kernel/core/subsys/human68k_bridge.c),
[`sos_bridge.c`](../../src/kernel/core/subsys/sos_bridge.c),
[`cpm_bridge.c`](../../src/kernel/core/subsys/cpm_bridge.c),
[`h68k_emu.c`](../../src/kernel/core/exec/h68k_emu.c).

These are the last four `mem_region_ptr_ref` callers. Converting
their signatures to `(page, off)` eliminates `ptr_ref` entirely,
allowing `mem_helper.h` and `subtle/` to be deleted as the final
step.

Each bridge today receives a `void *` from the eCPU memory model — but
the eCPU memory model is itself running on a user page tracked in
`user_pages[]`. The `void *` is *synthesised* from the user page just
to be passed across one function-call boundary, then re-encoded back
into `(page, off)` via `ptr_ref` immediately on the other side. The
round trip exists for no reason other than the legacy bridge signature.

The fix is to **change the bridge helper signatures** to take
`(page_id_t page, uint16_t off, size_t n)` directly. The eCPU memory
model already knows the tracked user page, so it produces the pair at
the source instead of fabricating a `void *` and throwing it away.
After the change, the bridges call `mod_vfs.fd_read(desc, page, off, n)`
directly and the user page is the destination of the I/O. **No
bounce buffer ever exists.**

Concretely:

- 3.1. Audit each bridge's `void *` parameters. Replace with
  `(page_id_t, uint16_t)` pairs everywhere they currently feed into a
  `mod_vfs.fd_read/fd_write` or `mem_region_page_read/write` call.
- 3.2. Update the eCPU memory-model glue (`h68k_emu.c` etc.) to compute
  `(page, off)` from the user-process base page directly, using the
  same arithmetic that `user_to_page` uses on the syscall side.
  *Do not* introduce a shared helper for this — each eCPU's memory
  model is the right place for its own architectural details.
- 3.3. Verify on qemu_m68k (Human68k DOS test suite) and on x68k
  hardware-style tests if practical. Bridges run on every userspace
  I/O syscall in those subsystems, so any regression surfaces fast.
- 3.4. **Delete `mem_helper.h` and `subtle/` directory.** After 3.1
  converts the bridge signatures, `mem_region_ptr_ref` has zero
  callers. Remove the file, remove the `#include` from
  `mem_region.h`, and delete the `subtle/` directory.

Whether the subsys directory should also relocate from `core/subsys/`
into `vfs/subsys/` is a separate question that would shrink `mod_vfs`
further (see Open Question §6). It is **not** a prerequisite for the
bounce-buffer fix and is tracked as a follow-up under the
[Source tree alignment Future Work](../kernel/kernel_modules.md) item.

#### Phase 4 — Documentation and lint

4.1. Update [`docs/kernel/kernel_modules.md`](../kernel/kernel_modules.md):
    - Remove `mem_region_page_linear` from the mod_core function table
      (line 128).
    - Remove the asterisk + "subtle workaround" footnote
      (lines 133–135).
    - Update the directory listing (line 261) — `subtle/` is gone.
    - Mark issue #48 as done in the Future Work list.

4.2. Update [`docs/kernel/memory_management.md §3.2 / §3.4 / §9`](../kernel/memory_management.md):
    - Drop the "Inline cursor helpers live in
      `kernel/common/subtle/mem_helper.h` (legacy)" sentence.
    - Remove the `mem_region_page_linear` row from the mod_core vtable
      table.
    - State explicitly that `void *` → `(page, off)` conversion is
      **not** a public utility (R2): only `user_to_page` exists, and
      it lives next to the syscall dispatcher.

4.3. Add the Page-Indexed I/O Invariant (R1–R4) as a new section in
    [`memory_management.md`](../kernel/memory_management.md), just
    after §9 "Page-Index Conversion Rules".

4.4. Add a CI lint to
    [`scripts/check_module_boundaries.sh`](../../scripts/check_module_boundaries.sh)
    that fails the build on:

    - `mem_region_page_linear` in
      `src/kernel/{vfs,core/subsys,syscall}/` (with `core/exec/` on the
      lint allowlist until Phase 5 lands).
    - `mem_region_ptr_ref` anywhere (the symbol is gone after Phase 2).

    These are cheap `grep` invocations and prevent regression. Ensure the lint
    allows the new `mem_region_kbuf_to_page` helper in `kernel/common/`.

#### Phase 5 — Core loader migration (deferred, separate proposal)

Out of scope for this proposal. Sites:
[`core/exec/exec.c`](../../src/kernel/core/exec/exec.c) (×2),
[`core/exec/elf16_loader.c`](../../src/kernel/core/exec/elf16_loader.c).

These are entangled with process lifecycle (`proc_alloc`,
`proc_setup_stack`, `proc_track_page_range`,
`image_release_owned_segments`) and need their own design pass to
unwind. The `ptr_ref` calls in those files continue to work via locally
inlined encoding with a `TODO(mem_region_wrapup Phase 5)` comment until
a follow-up proposal addresses them. Performance: a future Phase 5
would also remove the per-`execve` ELF segment bounce, saving roughly
one segment-sized memcpy per loaded binary.

---

### 5. Surface and Performance Impact

| Phase | mod_core delta | mod_vfs delta | Bounces removed | Files moved |
|---|---|---|---|---|
| 1 — blkdev page-indexed + devfs cast fix | 0 | 0 | 0 | 0 |
| 2 — single-page contract, cursor helper removal | 0 | 0 | 0 | 0 |
| 3 — bridge conversion + `mem_helper.h` deletion | 0 | 0 | **4** (subsys×3, h68k_emu) | 0 |
| 4 — docs/lint | 0 | 0 | 0 | 0 |
| 5 — core loader (deferred) | 0 | 0 | 2 (exec×1, elf16_loader×1) | 0 |
| **Net (this proposal)** | **0** | **0** | **4** | **0** |

Performance impact (Phase 3):

- Every Human68k / SOS / CP/M file I/O call saves one full-buffer
  `memcpy` per syscall. The bridges run on every userspace I/O syscall
  in those subsystems on m68k/x68k.
- File-data path through ufs/vfat/tmpfs is unchanged (already
  zero-bounce per §3.1).

Footprint impact (Phase 3, i16):

- Removes per-bridge stack-allocated transfer buffers in subsys.
- Estimated kernel data segment savings: a few hundred bytes
  (concrete figures TBD by inspection during Phase 3). Modest in
  absolute terms but meaningful against the [pcxt code
  budget](../../memory/project_pcxt_size.md) (~13 KB current headroom).
- Larger savings (`file_buf` scratch arrays in `exec.c` /
  `elf16_loader.c`) are deferred to Phase 5.

---

### 6. Open Questions

These need user input or short audits before Phase 1 starts:

1. ~~**Step 1.6 driver question (load-bearing)**~~ — resolved. The pcxt floppy
   driver will be relocated back to the core side (Option 1.6b), allowing
   `mem_region_page_linear` to be cleanly removed from `mod_core`.

2. ~~Cursor helper home~~ — resolved. Phase 1's single-page contract
   moves the cursor walk from VFS leaves up to the root callers. After
   Phase 1, the cursor helpers have no callers, and Phase 2 deletes
   them entirely with no replacement file. The 3–5 root callers each
   inline two lines of arithmetic, deliberately, with no shared
   symbol.

3. **Subsys relocation** (`core/subsys/` → `vfs/subsys/`): not required
   for this proposal. If landed alongside Phase 3 it would convert the
   bridges' `mod_vfs.fd_*` calls into intra-module calls and may
   permit further `mod_vfs.inc` shrinkage. Recommendation: track as a
   separate proposal under the
   [Source tree alignment Future Work](../kernel/kernel_modules.md)
   item.

4. **Phase ordering**: 1 → 2 → 3 → 4, with 5 deferred. Phase 1 is the
   structural change and benefits from a worktree branch
   (`isolation: worktree`) because it touches every storage backend on
   every target. Phase 2 is one commit and self-contained, lands once
   Phase 1 is merged so that loopback's `ptr_ref` is already gone.
   Phase 3 is a series of small, file-by-file commits. Phase 4 is
   mechanical follow-up. Phase 5 is a future proposal.

---

### 7. Related Documentation

- [Kernel Module System](../kernel/kernel_modules.md) — module boundary
  rules (R5, R6) and the Future Work list that frames this cleanup.
- [Memory Management](../kernel/memory_management.md) — `mem_region`
  public API (R3) and the page-index conversion rules.
- [PC Port Plan §7](pc_port.md) — the `user_to_page` discipline that
  this proposal extends to the rest of the kernel.
- [Issue #48](https://github.com/toyoshim-i/PiPAPo/issues/48) — closed
  by Phase 1.
