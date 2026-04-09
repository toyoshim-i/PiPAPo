## Kernel Stack Reduction on ia16

> **Status**: Proposal.
> Targets the removal of the temporary slot-2 skip workaround by reducing
> worst-case ia16 kernel stack use below the real 2 KB per-process budget.

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
- `lookup_walk_flags` path scratch bundle

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

1. `sys_getdents`
2. `elf16_load_vnode`
3. `sys_execve`
4. `namei`

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

The numbers below come from the current ia16 object code where available, and
from source-level struct sizes where the compiler frame alone is not enough to
assign the cost to a specific local.

#### 5.1 `getdents`

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

#### 5.2 `execve`

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

#### 6.3 Move `sys_execve` scratch bundle off-stack

Main locals:

- `path[128]`
- `argv_copy[65]` = about 130 B on ia16
- `argv_buf[128]`
- `old_user[64]` = 64 B because `page_id_t` is 8-bit on ia16
- `old_image` = about 132 B

Expected savings:

- about `582 B`

Expected new peaks:

- lookup side: about `1540 - 582 = 958 B`
- load side: about `1550 - 582 = 968 B`

Implementation preference:

- use one typed `kmem` scratch object

Reason:

- this is a structured bundle of metadata and small buffers
- a per-call page would waste most of the page

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
- `target[128]`
- `norm[128]`

Expected savings:

- `vfs_lookup_flags`: about `256 B`
- `lookup_walk_flags`: about `448 B`
- combined: about `704 B`

Expected new lookup-side peak:

- about `1540 - 704 = 836 B`

Implementation preference:

- use one typed `kmem` scratch object shared by the walk

Reason:

- the buffers are metadata/path scratch
- they fit naturally in one reusable bundle

### 7. Recommended Order

The order below maximizes certainty and headroom.

#### Phase 1: eliminate the known `getdents` overflow

- move `sys_getdents`'s `entries[32]` into a page-backed scratch page

Expected result:

- `getdents` stops being an unconditional stack overflow

#### Phase 2: shrink the loader-side `execve` peak

- move `elf16_load_vnode`'s `ehdr` + `phdrs[]` into one `kmem` scratch object
- move `elf16_load_from_headers`'s `zeros[256]` off-stack

Expected result:

- load-side `execve` peak drops from about `1550 B` to about `730-990 B`,
  depending on whether the zero buffer is folded into the same scratch object

#### Phase 3: shrink the syscall-side `execve` peak

- move `sys_execve` locals into one `kmem` scratch object

Expected result:

- both `execve` peaks drop by another `~582 B`

#### Phase 4: shrink `namei`

- move `vfs_lookup_flags` + `lookup_walk_flags` path buffers into one
  reusable scratch bundle

Expected result:

- lookup-side `execve` peak drops into the sub-1 KB range with good margin

### 8. Suggested Scratch Shapes

These are the intended shapes, not final APIs.

#### 8.1 `getdents` page scratch

One page-backed scratch page:

- holds up to one page of `struct dirent` entries
- reused only within the syscall
- copied to userspace with the existing `sys_copy_to_user` path

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

One fixed typed object:

- `char abs_buf[VFS_PATH_MAX]`
- `char normalized[VFS_PATH_MAX]`
- `char resolved[VFS_PATH_MAX]`
- `char comp[VFS_NAME_MAX + 1]`
- `char target[VFS_PATH_MAX]`
- `char norm[VFS_PATH_MAX]`

This bundle is large, so Phase 4 should only happen if Phases 1-3 still leave
insufficient headroom or if other path syscalls remain close to the canary.

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
   frames in the same sweep, or only after re-measuring the post-Phase-3
   high-water mark?
3. Once the largest stack locals move out, should the kernel canary panic also
   report the current syscall number to speed future regressions?
