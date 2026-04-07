## mem_region Wrap-Up: Deprecating `mem_helper.h` and Closing Issue #48

> **Status**: Proposal. Targets the cleanup of
> [`src/kernel/common/subtle/mem_helper.h`](../../src/kernel/common/subtle/mem_helper.h)
> and the residual `mem_region_page_linear` callers in fs/exec/subsys.

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
3. `mem_region_page_linear()` is still in the `mod_core` vtable
   ([`mod_core.inc:21`](../../src/kernel/common/mod/mod_core.inc),
   [`mod_core.h:136`](../../src/kernel/common/mod/mod_core.h)) and is
   documented as a "subtle workaround for blkdev DMA". The follow-up
   tracked in [issue #48](https://github.com/toyoshim-i/PiPAPo/issues/48)
   was supposed to convert blkdev to take `(page, off)` directly and let
   the entry be removed.
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
   already governs the VFS read/write path, and it is the prerequisite
   for ever removing `mem_region_page_linear` from `mod_core`. Issue #48
   has been pending since the blkdev relocation in commit `42a84ec`.

2. **Phase 2 — `mem_helper.h` deletion.** Small, self-contained,
   gated on Phase 1. The single-page contract from Phase 1 makes the
   cursor helpers caller-less, so Phase 2 simply removes them along
   with `ptr_ref` and the `subtle/` directory. No replacement file,
   no shared symbol.

3. **Phase 3 — Bounce-buffer elimination in
   `core/subsys/{human68k,sos,cpm}_bridge.c` and
   `core/exec/h68k_emu.c`.** These run on every userspace I/O syscall on
   m68k/x68k, so removing the per-call bounce is a measurable
   performance + footprint win.

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

#### R3. `mem_region_page_linear()` is a hardware-driver-only escape hatch

After this cleanup, `mem_region_page_linear` may be called only from
`src/kernel/{core,vfs}/driver/` at the exact site of a BIOS call,
DMA register write, or other hardware programming step. It is not part of
the cross-module surface. If a driver moves into the VFS module, it
becomes a target-private symbol of that module — not a `mod_*` entry.

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

This proposal **shrinks** the module surface by one entry
(`mem_region_page_linear`) and adds zero new ones.

#### R6. Module-relocation over interface growth

If two pieces of code need to communicate across a module boundary,
prefer moving one of them to the other's module over adding a `mod_*`
entry. Drivers in particular: a driver that backs a vnode or a
filesystem object belongs in `vfs/driver/`, not `core/driver/`. A
driver that is purely a service to mod_core (klog backend, page-pool
allocator backend) may stay in core.

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
syscall dispatcher / `fd_read`/`fd_write` / eCPU bridge). After
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

#### 3.3 `mem_region_page_linear` callers in non-driver code

A separate `Grep mem_region_page_linear src/kernel/{vfs,core/exec,core/subsys}/`
is required to enumerate. Every hit is by definition a violation of R3
after this cleanup, and is fixable by either:

- routing the buffer through `mem_region_page_read/write` instead, or
- moving the file to the appropriate driver module under `target/`.

The expected end state is **zero** matches in those subtrees.

---

### 4. Phased Plan

Phases land in numeric order **1 → 2 → 3 → 4**, with Phase 5 deferred to a
later proposal. Each leaves the tree building and tested on qemu_arm +
qemu_m68k + pcxt at minimum; floppy-using phases also test x68k.

#### Phase 1 — Close issue #48 (`blkdev` page-indexed signature) — **PRIORITY**

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

    The same rule applies to every `(page_id, off, count)`-shaped
    callee introduced by this proposal (vnode read/write,
    `mem_region_page_read/write`, blkdev read/write, the eCPU memory
    model glue in Phase 3): single-page semantics, partial-completion
    return value, no implicit page walking inside the leaf.

    The **root caller** — the syscall dispatcher in core, the eCPU
    bridge, or `fd_read/fd_write` itself — owns the loop. It computes
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

1.2. Update [`mod_core.h`](../../src/kernel/common/mod/mod_core.h)
    `MOD_FUNC` declarations for `blkdev_read` / `blkdev_write`. Indices
    in [`mod_core.inc`](../../src/kernel/common/mod/mod_core.inc) stay
    the same; `core_stubs.S` / `core_entries.S` regenerate
    automatically. Verify the new arg count fits the existing two-level
    stub register marshaling (see
    [`kernel_modules.md §i16 Segment Split`](../kernel/kernel_modules.md)).

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

1.5. Audit `mem_region_page_linear` callers globally. The expected end
    state is:

- Zero hits in `src/kernel/vfs/`, `src/kernel/core/exec/`,
  `src/kernel/core/subsys/`, `src/kernel/syscall/`.
- Hits only in `src/kernel/{core,vfs}/driver/` and possibly
  `src/arch/<arch>/` (MMU/MPU setup).

1.6. **Remove `mem_region_page_linear` from the `mod_core` vtable.**
    Delete the entries from [`mod_core.h`](../../src/kernel/common/mod/mod_core.h)
    and [`mod_core.inc`](../../src/kernel/common/mod/mod_core.inc).
    Renumber subsequent entries. Driver code in `target/` calls
    `mm_page_linear()` directly (no module boundary because target code
    links against the core image). Net change: **`mod_core` shrinks
    from 20 to 19 entries.**

If 1.6 is blocked because some target driver lives in the VFS module
(e.g. pcxt floppy) and therefore *does* cross a module boundary to
reach `mm_page_linear`, the resolution is **Option 1.6b**:

- **Option 1.6b**: relocate the driver back to a target-specific
  position that lives on the core side of the boundary. Per R6, prefer
  this over keeping the entry.

Based on an audit of the `pcxt` floppy driver: the driver currently casts
its buffer to a `uint16_t` for the BIOS call, implicitly relying on the buffer
living in the `DS=0` segment. With `(page, off)` args, it must start using
`mem_region_page_linear` to synthesize the physical address. Because the driver
currently lives in the VFS module, it would need to cross the module boundary.
To resolve this and successfully remove the `mod_core` entry, we will proceed
with Option 1.6b and relocate the hardware-specific floppy drivers out of
`vfs/driver/` and back into `target/<arch>/kernel/...` (core-side).

#### Phase 2 — Delete `mem_helper.h`

Goal: remove the apologetic header without introducing any replacement
file. Phase 1 already eliminated every cursor-helper caller by moving
the page walk up to the root callers; Phase 2 just collects the
remains.

Prerequisite: Phase 1 has landed. All 14 cursor-math call sites in
§3.1 have been simplified to single-page leaves, and the root callers
(syscall dispatcher, `fd_read`/`fd_write`, eCPU bridge) drive the page
walk inline with two lines of arithmetic each:

```c
uint16_t chunk = (PAGE_SIZE - off < remaining) ? (PAGE_SIZE - off)
                                               : (uint16_t)remaining;
/* … call leaf with (page, off, chunk) … */
size_t pos = (size_t)off + handled;
page += (page_id_t)(pos / PAGE_SIZE);
off   = (uint16_t)(pos % PAGE_SIZE);
```

There are roughly 3–5 root-caller sites. Each gets the arithmetic
inline next to a one-line comment naming the cursor it walks. **No
shared helper, no shared symbol, no shared header.** The same rule
that R2 applies to `ptr_ref` also applies here: small inline
arithmetic at deliberate sites is preferable to a public utility that
can be casually reached for.

2.1. Confirm zero remaining callers of `mem_region_page_chunk_len` and
    `mem_region_page_advance` after Phase 1. If any leaf still walks a
    cursor, it is a Phase 1 oversight — fix it before proceeding.

2.2. The `ptr_ref` symbol is replaced with a strictly scoped inline helper
    (e.g., `mem_region_kbuf_to_page`) in `kernel/common/`. Each surviving caller
    after Phase 1 and Phase 3 either:

    - is a Phase 5 deferral, in which case it inlines the encoding
      locally with a `TODO(mem_region_wrapup Phase 5)` comment, or
    - is legitimate metadata I/O (e.g., `vfs/fstab.c`, `ufs_buf`, `sector_buf`),
      which will use the new inline helper.

2.3. Delete [`src/kernel/common/subtle/mem_helper.h`](../../src/kernel/common/subtle/mem_helper.h).
    Remove the `#include` from
    [`mem_region.h`](../../src/kernel/core/mm/mem_region.h). If
    `subtle/` becomes empty, remove the directory.

2.4. Update [`memory_management.md §3.2`](../kernel/memory_management.md)
    and [`kernel_modules.md` directory listing](../kernel/kernel_modules.md)
    — remove the "(legacy)" framing and the `subtle/` entry.

After Phase 2, no `mem_helper.h`, no `page_cursor.h`, no shared
`ptr_ref` / cursor helper anywhere in the tree. The only places that
walk page cursors are the handful of root-caller loops, each with
inline arithmetic. The only places that encode kernel pointers as
`(page, off)` are `vfs/fstab.c` (R4-legitimate) and the Phase 5
TODO sites in `core/exec/`. No `mod_*` entries added.

#### Phase 3 — Bounce-buffer elimination in subsys + h68k_emu

Sites: [`human68k_bridge.c`](../../src/kernel/core/subsys/human68k_bridge.c),
[`sos_bridge.c`](../../src/kernel/core/subsys/sos_bridge.c),
[`cpm_bridge.c`](../../src/kernel/core/subsys/cpm_bridge.c),
[`h68k_emu.c`](../../src/kernel/core/exec/h68k_emu.c).

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
| 1 — blkdev page-indexed | **−1** (`mem_region_page_linear`, conditional on 1.6) | 0 | 0 | possibly 1 (driver re-home, see 1.6) |
| 2 — `mem_helper.h` deletion | 0 | 0 | 0 | 0 |
| 3 — subsys + h68k_emu | 0 | 0 | **4** (subsys×3, h68k_emu) | 0 |
| 4 — docs/lint | 0 | 0 | 0 | 0 |
| 5 — core loader (deferred) | 0 | 0 | 2 (exec×1, elf16_loader×1) | 0 |
| **Net (this proposal)** | **−1** | **0** | **4** | **0–1** |

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
