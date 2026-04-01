# PPAP Kernel Module System

## Problem

The PPAP kernel is growing (~177 KB code on ARM).  On i8086 real mode,
the 64 KB segment limit makes it impossible to link as a single binary.
But adding i16-specific `__far` annotations would pollute the portable
kernel source.

## Design: Platform-Agnostic Module System

A kernel module system across **all platforms**, not just i16.
Modules define clear API surfaces.  On 32-bit platforms, module calls
are direct function calls (zero overhead).  On i16, the same API
surfaces become far-call boundaries.

This is **not** a microkernel — no message passing, no server
processes.  It's a single-process plugin system with explicit API
surfaces, like Linux kernel modules but simpler.

### Module Architecture

```
  ┌─────────────────────────────────────────────────┐
  │              mod_core (core module)              │
  │  klog, kmem, mem_region, sched, uart, blkdev    │
  │                                                  │
  │  mod_vfs.*  ──►              mod_exec.*  ──►    │
  └──────────┬──────────────────────┬───────────────┘
             │                      │
       ┌─────▼──────┐        ┌─────▼──────┐
       │  mod_vfs    │        │  mod_exec  │
       │  vfs, namei │        │  exec,     │
       │  fd pool    │        │  loaders   │
       │  fs drivers │        │            │
       │  tty, pipe  │        │            │
       │             │        │            │
       │ mod_core.*  │        │ mod_core.* │
       └─────────────┘        └────────────┘
```

ALL cross-module calls go through `mod_*` interfaces.  No direct
function calls across module boundaries.  On i16, the `mod_*` struct
entries point to assembly stubs that handle the segment transition.
On other architectures, they point directly to the real functions.

Inline functions (spinlock, string) compile into each module's code
and do not cross module boundaries.

### Module Macros

Declaration (in `mod/mod_<name>.h`):

```c
MOD_DECLARE_BEGIN(vfs)
  MOD_FUNC(vfs, void, init, void)
  MOD_FUNC(vfs, int,  mount, const char *, ...)
MOD_DECLARE_END(vfs)
```

Definition (in the module's `.c` file), auto-generated from `.inc`:

```c
MOD_DEFINE_BEGIN(vfs)
#define MOD_VFS_ENTRY(name, idx)  MOD_IMPL(vfs, name)
#include "../common/mod/mod_vfs.inc"
#undef MOD_VFS_ENTRY
MOD_DEFINE_END()
```

Calling:

```c
mod_vfs.init();
int desc = mod_vfs.fd_open("/etc/fstab", O_RDONLY, 0);
```

| Platform | MOD_FUNC | Call overhead |
|----------|----------|--------------|
| ARM, m68k, RISC-V, Xtensa | struct of fn pointers (static init) | ~1 indirect call |
| i16 | struct of fn pointers (runtime stubs) | ~2 indirect calls |

### Naming Convention

Struct fields use `<group>_<action>` style, sorted alphabetically:
`fd_open`, `fd_read`, `mount_find`, `sched_yield`, `vnode_alloc`.

Real function names are prefixed with `<module>_`: `vfs_fd_open`,
`vfs_mount_find`, `core_blkdev_read`.  The `MOD_IMPL(vfs, fd_open)`
macro maps `.fd_open = vfs_fd_open`.

Reference counting uses `acquire`/`release` pairs:
`vnode_acquire`/`vnode_release`, `fd_acquire`/`fd_release`.

### Function Index Files (`.inc`)

Each module has a `.inc` file that is the **single source of truth**
for function names and indices:

```
src/kernel/common/mod/
  mod_core.inc    ← 23 functions
  mod_vfs.inc     ← 35 functions
  mod_exec.inc    ← 1 function
```

Both C headers (`_Static_assert`) and assembly stubs (`#include`)
use the same `.inc` file.  Entries are sorted alphabetically.
Adding a new function requires updating exactly two files:

1. `mod_<name>.h` — add `MOD_FUNC(...)` with types (in sorted position)
2. `mod_<name>.inc` — add `MOD_<NAME>_ENTRY(name, idx)` (in sorted position)

The `_Static_assert` catches struct/count mismatches at compile time.
Assembly stubs auto-generate from the `.inc` — they never get out
of sync.

### Modules

#### mod_core (23 functions)

Common services that all other modules depend on.

| Group | Functions |
|-------|-----------|
| Logging | `klog`, `klogf` |
| Slab allocator | `kmem_alloc`, `kmem_free`, `kmem_free_count`, `kmem_pool_init` |
| Region allocator | `mem_region_alloc`, `mem_region_free`, `mem_region_free_bytes`, `mem_region_total_bytes` |
| Page-indexed memory | `mm_page_alloc`, `mm_page_free`, `mm_page_read`, `mm_page_write` |
| Scheduler | `sched_get_ticks`, `sched_wakeup`, `sched_yield`, `svc_set_restart` |
| UART (bootstrap) | `uart_getc`, `uart_putc`, `uart_rx_avail` |
| Block device I/O | `blkdev_read`, `blkdev_write` |

**UART in core:** UART is theoretically a character device belonging
under VFS/devfs, but `klog` needs `uart_putc` before VFS is
initialized.  The bootstrap dependency requires it in core.

**kmem vs mem_region:** `kmem` is a sub-page slab allocator for
fixed-size kernel objects (vnodes, files).  `mem_region` is a
page-granularity allocator for process images and tmpfs data.
Both are needed — `mem_region` for a 138-byte vnode wastes 97%
of a 4 KB page.

**mm_page vs mem_region:** `mm_page_alloc` returns a `page_id_t`
(index, not pointer) for i16 segment safety.  `mm_page_read/write`
internally set up segment:offset pairs for cross-segment access.
On 32-bit, these are thin wrappers around memcpy.

#### mod_vfs (35 functions)

VFS layer, file descriptor pool, filesystem drivers, TTY.

| Group | Functions |
|-------|-----------|
| VFS core | `init`, `mount`, `umount`, `lookup`, `lookup_flags`, `lookup_parent`, `path_normalize` |
| Mount helpers | `mount_by_fstype`, `mount_find`, `mount_romfs`, `mount_ufs` |
| Vnode lifecycle | `vnode_alloc`, `vnode_acquire`, `vnode_release`, `vnode_read` |
| File descriptor pool | `fd_acquire`, `fd_fcntl`, `fd_fstat`, `fd_fstatfs`, `fd_getdents`, `fd_getdents64`, `fd_get_priv`, `fd_ioctl`, `fd_lseek`, `fd_open`, `fd_pipe_create`, `fd_poll`, `fd_pool_init`, `fd_read`, `fd_release`, `fd_stdio_desc`, `fd_stdio_init`, `fd_write` |
| Init helpers | `fstab_automount`, `tty_rx_notify` |

#### mod_exec (1 function)

| Group | Functions |
|-------|-----------|
| Process loading | `execve` |

### System-Wide File Descriptor Pool

Core owns a per-process `int16_t fd_map[FD_MAX]` in `pcb_t`.
VFS owns a system-wide `struct file fd_pool[FILE_MAX]` indexed by
descriptor ID.  VFS is process-agnostic — it never sees pid or pcb_t.

```
  Core (per-process)              VFS (system-wide, process-agnostic)
  ┌──────────────────┐            ┌──────────────────────────────┐
  │ pcb_t.fd_map[]   │            │ fd_pool[FILE_MAX]            │
  │  [0] → desc 0   ─┼───────►   │  [0] tty_stdin  (refcounted) │
  │  [1] → desc 1   ─┼───────►   │  [1] tty_stdout              │
  │  [2] → desc 2   ─┼───────►   │  [2] tty_stderr              │
  │  [3] → desc 5   ─┼───────►   │  [5] vnode+offset+ops        │
  │  [4] → -1 (empty)│            │                              │
  └──────────────────┘            └──────────────────────────────┘
```

Syscalls resolve `fd_map[fd]` → `desc_id`, then call
`mod_vfs.fd_read(desc_id, ...)`.  Fork copies `fd_map[]` and
calls `mod_vfs.fd_acquire(desc_id)` to bump refcount.  Exit
iterates `fd_map[]` calling `mod_vfs.fd_release(desc_id)`.

### i16 Segment Split (PC/XT)

On i16, all modules share **DS=0** for data access.  Each module's
code (.text) lives in its own segment (separate CS).  Cross-module
calls use `lcall`/`lret` to switch CS; DS stays unchanged.

```
  Linear address space:
  0x0000─0x0500   BIOS/DOS work area
  0x0500─0x0600   mod_info_t (stage2 → kernel handoff)
  0x0600─0x9FFF   Core code + BSS + stack (CS=0)
  0xA000─0xBFFF   VFS data (.rodata + .data + .bss, DS=0)
  0xC000─0x9EFFF  Page pool (user memory)
  0x10000+        VFS code segment (CS=0x1000)
```

**Two-level stub pattern:**

```asm
; Caller-side stub (in caller's segment):
vfs_init:
    pop  %ax                    ; near_ret_IP
    push %cs                    ; save caller CS for lret
    push %ax                    ; save near_ret_IP
    ljmp *%ss:vfs_fptrs + 0    ; far jump to VFS entry

; Target-side stub (in VFS segment):
vfs_init_entry:
    cli
    pop  %ax / pop  %bx         ; save far return frame
    mov  %ax, %ss:saved_ip
    mov  %bx, %ss:saved_cs
    sti
    xor  %ax, %ax
    mov  %ax, %ds               ; restore DS=0
    call vfs_init               ; near call to real function
    cli
    push %ss:saved_cs
    push %ss:saved_ip
    sti
    lret                        ; far return to caller
```

**ia16-elf-gcc behaviour:**
- The compiler uses **SS for all data access** (`%ss:` prefix).
  DS is a scratch register.
- SS must always = 0 (the shared data segment invariant).
- Assembly stubs must use `%ss:` for any data access.

**Two-pass link for shared data:**
Core is linked first.  `gen_core_exports.sh` extracts data/BSS
symbol addresses into `core_exports.ld`.  VFS links with this
symbol file so the linker resolves `proc_table`, `current_core`,
etc. to correct DS=0 addresses.

**No `--unresolved-symbols=ignore-all`:** Both core and VFS link
with strict symbol resolution.  Any cross-module boundary violation
is a **compile-time error**.

### Directory Structure

```
src/kernel/
  common/mod/
    module.h          ← module system macros (MOD_FUNC etc.)
    mod_core.h        ← core module interface (23 functions)
    mod_core.inc      ← core function index (alphabetical)
    mod_vfs.h         ← VFS module interface (35 functions)
    mod_vfs.inc       ← VFS function index (alphabetical)
    mod_exec.h        ← exec module interface (1 function)
    mod_exec.inc      ← exec function index
    mod_core.c        ← core struct initializer (32-bit)
  vfs/
    vfs.h             ← PRIVATE: types + internal declarations
    vfs.c             ← VFS implementation + MOD_DEFINE
    vfs_types.h       ← shared types (vnode_t, mount_entry_t)
    namei.c           ← path resolution (VFS-internal)
  fd/
    fd.c              ← system-wide fd pool + VFS bridge ops
    fd.h              ← VFS-internal fd declarations
    file.h            ← struct file definition
    pipe.c            ← pipe implementation + fd_pipe_create
    tty.c             ← TTY driver + line discipline
  exec/
    exec.c            ← exec coordinator + MOD_DEFINE
    loader.c          ← binary format detection
  fs/
    romfs.c, tmpfs.c, devfs.c, procfs.c, ufs.c, vfat.c, fstab.c

src/target/pcxt/stubs/
  core_stubs.S        ← VFS-side caller stubs for core (auto-gen from .inc)
  core_entries.S      ← core-side target entry stubs (auto-gen from .inc)
  core_mod_init.c     ← VFS-side mod_core struct (i16 only)
  vfs_stubs.S         ← core-side caller stubs for VFS (auto-gen from .inc)
  vfs_entries.S       ← VFS-side target entry stubs (auto-gen from .inc)
  vfs_header.S        ← VFS module header (entry point table)
  vfs_mod_init.c      ← core-side mod_vfs struct (i16 only)
```

### Boundary Rules

Two strict rules:

1. **Inward rule:** Files outside `vfs/` MUST NOT include headers
   inside `vfs/` (e.g. `vfs/vfs.h`).  They use `mod/mod_vfs.h`.

2. **Outward rule:** Files inside `vfs/` MUST NOT include headers
   inside other module directories (e.g. `exec/exec.h`).  They use
   `mod/mod_exec.h`.

**Allowed includes from any module:**
- `mod/mod_*.h` — public module interfaces
- `common/*` or standard headers — shared utilities
- Headers within the same module directory

On i16, the linker enforces these boundaries — unresolved symbols
from the wrong module cause build errors.

### Design Decisions

**Type safety:** `MOD_FUNC` generates struct fields with full type
signatures.  Any mismatch between the struct and the implementation
is a compile error.

**UART in core:** Bootstrap dependency — klog needs uart_putc before
VFS exists.  VFS consumers access UART through mod_core exports.

**API surface minimization:** Module interfaces expose the smallest
possible set of functions.  `fstab_automount` combines parse + mount
behind the boundary.  `mount_by_fstype` keeps fs ops tables in VFS.
`fd_pool_init` and `tty_rx_notify` are VFS-internal init helpers
exposed only because core calls them at boot.

**Loading:** Stage2 loads all modules at boot time at fixed addresses.
No runtime module loading — simpler and sufficient.

### Future Work

- **mod_signal** — signal delivery (currently direct calls)
- **mod_subsys** — eCPU emulators + personality bridges
- **mm_page / mem_region integration** — unify page-indexed and
  region-based allocators behind a single API surface
- **Source tree alignment** — restructure `src/kernel/` so each
  module's directory matches its boundary exactly (e.g. move
  `fd/`, `fs/` into `vfs/`; move `proc/`, `syscall/`, `mm/` into
  `core/`).  Currently the boundary is defined by the build
  system and `.inc` files, not by directory structure
