## Kernel Stack Reduction on ia16

> **Status**: In progress.
> The slot-2 skip workaround has already been removed. `getdents`,
> loader-side `execve`, and both `namei` reduction steps are landed. A
> `sys_execve` reduction is now in review: the safe ia16 version uses shared
> low-memory scratch and measures well, while a rejected page-backed attempt
> proved that generic page-pool pointers are still unsafe for string scratch on
> ia16.

### 1. Background

The pcxt port gives each PCB slot a dedicated 2 KB kernel stack in DS=0:

- slot 0: `0xE000-0xE7FF`
- slot 1: `0xE800-0xEFFF`
- slot 2: `0xF000-0xF7FF`
- slot 3: `0xF800-0xFFFF`

The temporary workaround was to skip slot 2 and let slot 3 spill into that
space as a virtual 4 KB stack. That workaround masked a real kernel-stack
problem and reduced process capacity.

Once the workaround is removed, the failure is reproducible again:

```text
SCHED: starting scheduler
PANIC: kernel stack overrun  slot=1 base=0x0000e800 got_base=0x0000ca57  topG0=0x00000000 topG1=0x00000000  pid=2 comm=sh
```

This report means:

- slot 1's base canary is still intact
- slot 1's top guard was overwritten
- the next higher stack overflowed downward into slot 1

With `pid=2 comm=sh`, the practical interpretation is that the shell running
in the next slot overflowed its own 2 KB kernel stack.

The known heavy syscall cases are:

- `getdents`
- `execve`

This document audits their current call chains, estimates the current peak
stack use, and proposes the reduction order.

### 2. Design Goal

The goal is not to increase the ia16 kernel-stack budget. The goal is:

- remove the slot-2 skip workaround
- keep the real 2 KB per-process stacks
- reduce worst-case syscall stack use until shell startup and common VFS paths
  stay comfortably below 2 KB

The preferred tactic is:

- move large stack locals into heap-backed scratch when the object is small and
  typed
- move large transient buffers into page-backed scratch when the data is bulky
  or page-oriented

### 3. Allocation Rules

The audit suggests two different scratch styles.

#### R1. Use `kmem` for small typed scratch objects

Use a slab pool when the object:

- has a fixed small size
- is metadata, not bulk data
- is naturally accessed through a typed struct
- should be reused without per-call page churn

Good candidates:

- `sys_execve` scratch bundle (`path`, `argv_copy`, `argv_buf`, rollback state)
- `elf16_load_vnode` header bundle (`ehdr`, `phdrs`)
- `lookup_walk_flags` symlink scratch bundle

#### R2. Use a page-backed scratch page for large bulk buffers

Use `mem_region` page-backed scratch when the object:

- is hundreds or thousands of bytes
- is copied to or from page-oriented APIs anyway
- is naturally bounded by one page

Good candidates:

- `sys_getdents` temporary `struct dirent[]` batch
- any future large pathname or directory-entry bounce buffer

#### R3. One scratch object per hot path

Do not replace one large stack frame with many separate heap allocations.
Bundle related locals into one scratch object where possible:

- one `execve_scratch`
- one `elf16_load_vnode_scratch`
- one `namei_walk_scratch`

That keeps the failure surface and cleanup logic simple.

#### R4. Reduce the biggest single frame first

The first reductions should target the most certain wins:

1. `sys_getdents` (done)
2. `elf16_load_vnode` / `elf16_load_from_headers` (done)
3. `namei`
4. `sys_execve`

The first `namei` sub-step is to move the symlink-only `target[]` and
`norm[]` buffers into a small VFS-owned `kmem` pool. That keeps pathname
scratch dereferenceable on ia16 and avoids introducing page-backed string
helpers into the path walker.

### 4. Audited Call Chains

#### 4.1 `getdents`

Current pcxt/UFS path:

```text
sys_getdents
  -> mod_vfs.fd_getdents
    -> vfs_fd_getdents
      -> ufs_readdir_locked
        -> ufs_readdir
```

References:

- [`src/kernel/core/syscall/sys_fs.c`](../../src/kernel/core/syscall/sys_fs.c)
- [`src/kernel/vfs/fd.c`](../../src/kernel/vfs/fd.c)
- [`src/kernel/vfs/ufs.c`](../../src/kernel/vfs/ufs.c)

#### 4.2 `execve`

The ia16 fast path has two distinct peaks.

Lookup side:

```text
sys_execve
  -> exec_execve
    -> mod_vfs.lookup
      -> vfs_lookup_flags
        -> lookup_walk_flags
          -> ufs_lookup
```

Load side:

```text
sys_execve
  -> exec_execve
    -> elf16_load_vnode
      -> elf16_load_from_headers
```

References:

- [`src/kernel/core/syscall/sys_proc.c`](../../src/kernel/core/syscall/sys_proc.c)
- [`src/kernel/core/exec/exec.c`](../../src/kernel/core/exec/exec.c)
- [`src/kernel/core/exec/elf16_loader.c`](../../src/kernel/core/exec/elf16_loader.c)
- [`src/kernel/vfs/namei.c`](../../src/kernel/vfs/namei.c)
- [`src/kernel/vfs/ufs.c`](../../src/kernel/vfs/ufs.c)

### 5. Current Stack Use

The numbers below are split into:

- the original audited peaks that motivated the work
- the current post-landing numbers where reductions have already been merged

Measured values come from the current ia16 object code where available. Source
estimates are used only where the compiler frame alone is not enough to assign
the cost to a specific local.

#### 5.1 `getdents`

Original audited state:

| Function | Current frame | Notes |
| --- | ---: | --- |
| `sys_getdents` | 2240 B | Exact ia16 frame. Dominated by `struct dirent entries[32]`. |
| `vfs_fd_getdents` | 6 B | Exact ia16 frame. Negligible. |
| `ufs_readdir` | ~140-160 B | `ufs_inode_t dir_inode` + per-entry `ufs_inode_t child` + loop vars. |

Key source facts:

- `struct dirent` is 70 B on ia16:
  - `uint32_t d_ino`
  - `uint8_t d_type`
  - `char d_name[65]`
- `entries[32]` therefore costs about `32 * 70 = 2240 B`
- `ufs_inode_t` is 64 B

Estimated `getdents` peak:

- about `2390-2430 B`

This already exceeds the 2 KB kernel-stack budget before any deeper path gets
interesting.

Current landed state:

| Function | Current frame | Notes |
| --- | ---: | --- |
| `sys_getdents` | ~0 B local frame | No large local array remains. |
| `vfs_fd_getdents` | 300 B | Small `struct dirent entries[4]` batch streamed to destination pages. |
| `ufs_readdir` | ~140-160 B | unchanged |

Current `getdents` peak:

- about `440-460 B`

This is comfortably below the 2 KB budget and is no longer the primary risk.

#### 5.2 `execve`

Original audited state:

| Function | Current frame | Notes |
| --- | ---: | --- |
| `sys_execve` | 590 B | Exact ia16 frame. |
| `exec_execve` | 86 B | Exact ia16 frame. |
| `vfs_lookup_flags` | 262 B | Exact ia16 frame. |
| `lookup_walk_flags` | 462 B | Exact ia16 frame. |
| `ufs_lookup` | ~130-150 B | Two 64 B inode locals plus small scalars. |
| `elf16_load_vnode` | 574 B | Exact ia16 frame. |
| `elf16_load_from_headers` | 300 B | Exact ia16 frame. |

Important nuance:

- the lookup side and the load side are separate peaks
- they do not all stack at once
- each peak is still too close to the 2 KB budget for comfort

Estimated lookup-side peak:

- `sys_execve + exec_execve + vfs_lookup_flags + lookup_walk_flags + ufs_lookup`
- about `590 + 86 + 262 + 462 + 140 = 1540 B`

Estimated load-side peak:

- `sys_execve + exec_execve + elf16_load_vnode + elf16_load_from_headers`
- about `590 + 86 + 574 + 300 = 1550 B`

This explains why `execve` is dangerous even though no single frame is as bad
as `sys_getdents`.

Current landed state:

| Function | Current frame | Notes |
| --- | ---: | --- |
| `sys_execve` | 590 B | landed baseline before Phase 4 |
| `exec_execve` | 86 B | unchanged |
| `vfs_lookup_flags` | 136 B | landed reduction |
| `lookup_walk_flags` | 206 B | landed reduction |
| `ufs_lookup` | ~130-150 B | unchanged |
| `elf16_load_vnode` | 58 B | landed reduction |
| `elf16_load_from_headers` | 140 B | landed reduction |

Current estimated peaks:

- lookup side: about `590 + 86 + 136 + 206 + 140 = 1158 B`
- load side: about `590 + 86 + 58 + 140 = 874 B`

So the load-side `execve` pressure is largely solved. The remaining big target
was the lookup side (`namei`), not the loader. With both `namei` steps landed,
the next decision is whether the remaining `sys_execve` frame is worth further
work.

Current Phase 4 candidate:

| Function | Current frame | Notes |
| --- | ---: | --- |
| `sys_execve` | 142 B | ia16-only shared low-memory scratch for `path`, `argv_copy`, and `argv_buf` |

Candidate estimated peaks:

- lookup side: about `142 + 86 + 136 + 206 + 140 = 710 B`
- load side: about `142 + 86 + 58 + 140 = 426 B`

This candidate materially improves kernel-stack headroom, but it consumes about
392 B of shared DS=0 data and therefore needs review together with the core
image budget.

### 6. Expected Impact Per Optimization

#### 6.1 Stream `getdents` directly to the destination pages

Original local:

- `struct dirent entries[32]` in
  [`src/kernel/core/syscall/sys_fs.c`](../../src/kernel/core/syscall/sys_fs.c)

Expected savings:

- about `2240 B`

Expected new `getdents` peak:

- about `300-450 B`

Implementation preference:

- change `fd_getdents` to accept `(page_id_t page, uint16_t off, size_t count)`
  and stream small `struct dirent` batches directly into the destination pages

Reason:

- this removes the syscall bounce buffer completely instead of relocating it
- it aligns `getdents` with the page/off data path already used by `read` and
  `write`
- the remaining VFS-side batch can stay small and bounded

Status:

- landed

#### 6.2 Move `elf16_load_vnode` ELF header scratch off-stack

Current locals:

- `elf32_ehdr_t ehdr` = 52 B
- `elf32_phdr_t phdrs[16]` = 512 B

Expected savings:

- about `564 B`

Expected new load-side peak:

- about `1550 - 564 = 986 B`

Implementation preference:

- use one typed `kmem` scratch object

Reason:

- the data is metadata
- the size is fixed
- it is a natural typed bundle

Status:

- superseded by a smaller landed change
- the final implementation reads one program header at a time instead of
  allocating a separate `kmem` scratch bundle

#### 6.3 Reduce the syscall-side `execve` peak

Main locals:

- `path[128]`
- `argv_copy[65]` = about 130 B on ia16
- `argv_buf[128]`
- `old_user[64]` = 64 B because `page_id_t` is 8-bit on ia16
- `old_image` = about 132 B

Original estimate:

- about `582 B`

Expected new peaks:

- lookup side: about `1540 - 582 = 958 B`
- load side: about `1550 - 582 = 968 B`

Current experiment result:

- moving only the rollback snapshot (`old_user[]` + `old_image`) into one
  temporary page reduced `sys_execve` from `590 B` to `526 B`
- actual win: `64 B`

Implementation preference:

- do not prioritize the rollback snapshot alone
- if `sys_execve` is revisited, target the remaining path/argv scratch as a
  bundle or reconsider the whole lookup-side peak first

Reason:

- this is a structured bundle of metadata and small buffers
- a per-call page would waste most of the page

Status:

- investigated
- not landed
- currently deprioritized behind `namei`

Current follow-up candidate:

- on ia16 only, move `path`, `argv_copy`, and `argv_buf` into one shared
  low-memory scratch object
- measured `sys_execve`: `590 B -> 142 B`

Rejected approach:

- a page-backed scratch object looked attractive on paper, but it caused
  immediate pcxt boot corruption because the string scratch was then accessed
  through generic page-pool pointers above 64 KB
- this is the same segment-safety rule already established by the i16 memory
  work: bulk page storage is fine through page APIs, but string-oriented C
  code still needs dereferenceable low-memory pointers

#### 6.4 Move the `elf16_load_from_headers` zero buffer off-stack

Current local:

- `uint8_t zeros[256]`

Expected savings:

- about `256 B`

Expected new load-side peak:

- about `1550 - 256 = 1294 B`

Implementation preference:

- keep it simple: either static const zero source or move it into the
  `elf16_load_vnode` scratch object

Reason:

- it is not process-specific state
- it does not need its own page

#### 6.5 Move `namei` path scratch off-stack

Current locals in `vfs_lookup_flags`:

- `abs_buf[128]`
- `normalized[128]`

Current locals in `lookup_walk_flags`:

- `resolved[128]`
- `comp[64]`
- `target[128]` (moved off-stack in the landed first sub-step)
- `norm[128]` (moved off-stack in the landed first sub-step)

Expected savings:

- `vfs_lookup_flags`: about `256 B`
- `lookup_walk_flags`: about `256 B` from the symlink-only scratch
- combined full target: about `512 B`

Expected new lookup-side peak:

- about `1540 - 256 = 1284 B` after the landed first sub-step
- about `1540 - 512 = 1028 B` after the remaining `vfs_lookup_flags` work

Implementation preference:

- first move the symlink-only scratch into a small typed `kmem` pool
- then reduce `vfs_lookup_flags()` without assuming in-place
  `path_normalize()` support

Reason:

- the buffers are metadata/path scratch
- the symlink branch was the highest-confidence isolated reduction

Status:

- both sub-steps landed
- `lookup_walk_flags`: `462 B -> 206 B`
- `vfs_lookup_flags`: `262 B -> 136 B`
- lookup-side `execve` peak: `~1540 B -> ~1158 B`
- the first landed step also required a pcxt-specific fix: the VFS module's
  upper DS BSS (`0xC000..0xDFFF`) must be cleared in `target_early_init()`
  because stage2 cannot clear that range while it still executes from
  `0xC000`

### 7. Recommended Order

The order below maximizes certainty and headroom.

#### Phase 1: eliminate the known `getdents` overflow

- move `sys_getdents`'s `entries[32]` into a page-backed scratch page

Expected result:

- `getdents` stops being an unconditional stack overflow

Status:

- done

#### Phase 2: shrink the loader-side `execve` peak

- move `elf16_load_vnode`'s `ehdr` + `phdrs[]` into one `kmem` scratch object
- move `elf16_load_from_headers`'s `zeros[256]` off-stack

Expected result:

- load-side `execve` peak drops from about `1550 B` to about `730-990 B`,
  depending on whether the zero buffer is folded into the same scratch object

Actual landed result:

- `elf16_load_vnode`: `574 B -> 58 B`
- `elf16_load_from_headers`: `300 B -> 140 B`
- load-side `execve` peak: `~1550 B -> ~874 B`

Status:

- done

#### Phase 3: shrink `namei`

Phase 3 is now split into two smaller steps.

Phase 3.1, landed:

- move the symlink-only `target[]` + `norm[]` scratch in
  `lookup_walk_flags()` into a small VFS-owned `kmem` pool
- fix the underlying pcxt VFS-module BSS initialization hole that this
  exposed by clearing the upper VFS DS window in `target_early_init()`

Actual result:

- `lookup_walk_flags`: `462 B -> 206 B`
- lookup-side `execve` peak: `~1540 B -> ~1284 B`

Phase 3.2, landed:

- reduce `vfs_lookup_flags()` to one stack path buffer
- keep absolute paths on the non-aliasing `path_normalize(path, normalized)`
  path
- keep the joined relative-path case on the in-place normalization path

Actual result:

- `vfs_lookup_flags`: `262 B -> 136 B`
- lookup-side `execve` peak: `~1284 B -> ~1158 B`
- `pcxt` boot still reaches `INIT: pid=1 loaded`

Reason for priority:

- the lookup side is still the dominant `execve` peak
- the loader side is already below 1 KB
- the attempted `sys_execve` snapshot-only reduction was much smaller than
  expected

#### Phase 4: revisit `sys_execve` only if needed

- bundle the remaining path/argv scratch only if post-`namei` measurements
  still show insufficient headroom

### 8. Suggested Scratch Shapes

These are the intended shapes, not final APIs.

#### 8.1 `getdents` direct page stream

Landed design:

- syscall resolves the destination user buffer to `(page, off)`
- `fd_getdents` writes small `struct dirent` batches directly into those pages
- no syscall-local or page-backed bounce buffer remains

#### 8.2 `execve_scratch`

One fixed typed object:

- `char path[VFS_PATH_MAX]`
- `const char *argv_copy[EXEC_ARGV_MAX + 1]`
- `char argv_buf[EXEC_ARG_BYTES_MAX]`
- `page_id_t old_user[USER_PAGES_MAX]`
- `proc_image_t old_image`

#### 8.3 `elf16_load_vnode_scratch`

One fixed typed object:

- `elf32_ehdr_t ehdr`
- `elf32_phdr_t phdrs[ELF16_MAX_PHDRS]`
- optional folded zero chunk if that keeps the code simpler

#### 8.4 `namei_walk_scratch`

Landed Phase 3.1 shape:

- `char target[VFS_PATH_MAX]`
- `char norm[VFS_PATH_MAX]`

Landed Phase 3.2 result:

- `vfs_lookup_flags()` now uses one stack path buffer
- absolute paths normalize into that buffer without aliasing the input
- relative paths still reuse the same buffer after path joining

### 9. Acceptance Criteria

The workaround is considered removable for good when:

- slot 2 is allocatable again
- shell startup no longer trips the kernel-stack canary
- `getdents` no longer has a frame above 2 KB
- `execve` peaks stay below the 2 KB budget with meaningful headroom
- the remaining high-water mark is measured on real pcxt boot/runtime, not
  only inferred from objdump

### 10. Open Questions

1. Should the scratch objects be global pools with a small fixed count, or
   per-subsystem static storage guarded by existing locks?
2. Is it worth addressing `sys_uname` and the other medium-large path/sysinfo
   frames in the same sweep, or only after re-measuring the post-`namei`
   high-water mark?
3. Once the largest stack locals move out, should the kernel canary panic also
   report the current syscall number to speed future regressions?
