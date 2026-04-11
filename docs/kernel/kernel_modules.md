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
  │  exec, loaders                                   │
  │                                                  │
  │  mod_vfs.*  ──►                                  │
  └──────────┬──────────────────────────────────────┘
             │
       ┌─────▼──────┐
       │  mod_vfs    │
       │  vfs, namei │
       │  fd pool    │
       │  fs drivers │
       │  tty, pipe  │
       │             │
       │ mod_core.*  │
       └─────────────┘
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
  mod_core.inc    ← 15 functions
  mod_vfs.inc     ← 39 functions
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

#### mod_core (15 functions)

Common services that all other modules depend on.

| Group | Functions |
|-------|-----------|
| Slab allocator | `kmem_alloc`, `kmem_free`, `kmem_free_count`, `kmem_pool_init` |
| Region allocator | `mem_region_alloc`, `mem_region_free`, `mem_region_free_bytes`, `mem_region_total_bytes` |
| Page-indexed memory | `mem_region_page_read`, `mem_region_page_write` |
| Scheduler | `sched_get_ticks`, `sched_wakeup`, `sched_switch`, `svc_set_restart` |
| Subsystem | `subsys_read_proc` |

**kmem vs mem_region:** `kmem` is a sub-page slab allocator for
fixed-size kernel objects (vnodes, files).  `mem_region` is a
page-granularity allocator for process images and tmpfs data.
Both are needed — `mem_region` for a 138-byte vnode wastes 97%
of a 4 KB page.

**mm_page vs mem_region:** `mm_page_alloc` returns a `page_id_t`
(index, not pointer) for i16 segment safety.  `mm_page_read/write`
internally set up segment:offset pairs for cross-segment access.
On 32-bit, these are thin wrappers around memcpy.

#### mod_vfs (39 functions)

VFS layer, file descriptor pool, filesystem drivers, TTY.

| Group | Functions |
|-------|-----------|
| VFS core | `init`, `mount`, `umount`, `lookup`, `lookup_flags`, `lookup_parent`, `path_normalize` |
| Mount helpers | `mount_by_fstype`, `mount_find`, `mount_romfs`, `mount_ufs` |
| Vnode lifecycle | `vnode_alloc`, `vnode_acquire`, `vnode_release`, `vnode_read`, `vnode_readlink`, `vnode_stat` |
| File descriptor pool | `fd_acquire`, `fd_fcntl`, `fd_fstat`, `fd_fstatfs`, `fd_getdents`, `fd_getdents64`, `fd_get_priv`, `fd_ioctl`, `fd_lseek`, `fd_open`, `fd_pipe_create`, `fd_poll`, `fd_pool_init`, `fd_read`, `fd_release`, `fd_stdio_desc`, `fd_stdio_init`, `fd_write` |
| Logging | `klogf` |
| Notification | `notify` |
| Init helpers | `fstab_automount`, `tty_rx_notify` |

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

On i16, all modules share a single **SS=0** kernel data segment for
all data access.  Each module's code (.text) lives in its own code
segment (separate CS).  Cross-module calls use `lcall`/`lret` to
switch CS; SS stays at 0.

#### Shared SS=0 data layout

The first 64 KB of physical memory is the shared kernel data
segment.  The layout below is enforced by
[`pcxt_kernel.ld`](../../src/target/pcxt/kernel/core/pcxt_kernel.ld) and
[`pcxt_vfs.ld`](../../src/target/pcxt/kernel/vfs/pcxt_vfs.ld).

```
  SS=0 linear address space (first 64 KB):
  0x00000-0x004FF   IVT + BIOS data area (real-mode)
  0x00500-0x005FF   mod_info_t handoff (stage2 → kernel, MOD_INFO_ADDR)
  0x00600-0x09FFF   Core .text / .rodata / .data / .bss
                    (.text is also runtime-copied to a far CS for
                    code execution; data lives only here)
  0x0A000-0x0DFFF   Reserved for VFS .rodata / .data / .bss
  0x0E000-0x0EFFF   Per-process kernel stacks (PROC_MAX × 1 KB,
                    currently 4 × 1 KB)
```

Sanity checks in `pcxt_kernel.ld` enforce that core BSS does not
overflow into the VFS reserved range, and `pcxt_vfs.ld` enforces
that VFS data does not overflow into the kernel stack range.

#### Far code segments (CS, runtime-assigned)

Stage2 loads each module twice: the core .text is copied once into
the SS=0 image at 0x0600 (so the linker can resolve all
SS-relative data accesses) and again into a far code segment for
execution.  VFS .text is loaded only into a far code segment.

The actual far CS values are dynamic — stage2 chooses them based
on image size and writes them into `mod_info_t`.  Typical layout
for a current build:

```
  Far code segments (above SS=0 data):
  CS=0x1000+        Core .text (far copy, ~34 KB → ends ~0x18FE7)
  CS=0x1900+        VFS  .text (~33 KB → ends ~0x21000)
  0x22000+          Page pool starts here (handed off via
                    mod_info[2].segment, see target_pcxt.c)
```

The page pool base is **not** a fixed constant — it follows the
VFS code segment and so depends on actual built sizes.  The boot
log line `MM: pages 0x000XXXXX-...` shows the runtime base.

#### Two-level far-call stub pattern

Cross-module calls go through a pair of stubs: a caller-side stub
in the caller's segment, and a target-side entry stub in the
callee's segment.  Both directions exist:

| Direction | Caller stub | Target entry stub |
|---|---|---|
| VFS → core | [`core_stubs.S`](../../src/target/pcxt/kernel/common/stubs/core_stubs.S) (linked into VFS) | [`core_entries.S`](../../src/target/pcxt/kernel/common/stubs/core_entries.S) (linked into core) |
| core → VFS | [`vfs_stubs.S`](../../src/target/pcxt/kernel/common/stubs/vfs_stubs.S) (linked into core) | [`vfs_entries.S`](../../src/target/pcxt/kernel/common/stubs/vfs_entries.S) (linked into VFS) |

Both directions share the same shape (showing core_stubs / core_entries):

```asm
; Caller-side stub (in caller's CS).  The compiler emits a near
; call to this label via mod_core.<func> (a near function pointer
; in the shared data segment).  The stub converts the near frame
; into a far frame and ljmps to the entry stub.
\name:
    pop  %ax                          ; near_ret_IP
    push %cs                          ; caller CS for the eventual lret
    push %ax                          ; near_ret_IP
    ljmp *%ss:core_fptrs + \idx*4    ; far jump to entry stub

; Target-side entry stub (in callee's CS).
\name\()_entry:
    cli
    pop  %ax                          ; near_ret_IP from far frame
    pop  %dx                          ; caller_CS from far frame
    mov  %ax, %ss:saved_ip
    mov  %dx, %ss:saved_cs
    sti
    xor  %dx, %dx
    mov  %dx, %ds                     ; restore DS=0 (DS is scratch)
    call \name                        ; near call to real C function
    cli
    xor  %dx, %dx
    mov  %dx, %ds                     ; restore DS=0 again (callee may
                                      ; have clobbered DS)
    push %ss:saved_cs
    push %ss:saved_ip
    sti
    lret                              ; far return to caller
```

**ABI notes embedded in the stub** (see asm comments for details):

- **AX** holds the return value — the stub must not clobber AX
  after the C function returns.
- **BX, SI, DI, BP** are callee-saved — the stub must not use them
  as scratch around the call.
- **CX, DX** are caller-saved — DX is therefore the scratch register
  the stub uses to shuffle the far-return frame and restore DS.
- The stub uses **static** `saved_ip` / `saved_cs` words (not the
  stack), so it is **not reentrant**.  Interrupts are disabled
  around the save/restore to prevent a nested far call (e.g. from
  a timer ISR) from overwriting the saved values.
- **DS is scratch** in ia16 small model: the compiler uses SS for
  all data access (`%ss:` prefix), DS is freely clobbered.  The
  entry stub resets DS to 0 both before the call (in case the
  caller left DS dirty) and after (in case the callee did).
- **32-bit return values** travel in DX:AX.  The entry stub
  preserves DX across the post-call DS restore by using CX
  (also caller-saved) as scratch for the `xor / mov DS` sequence.

#### Patching the far-pointer tables

Each direction has a far-pointer table (`core_fptrs` for VFS→core,
`vfs_fptrs` for core→VFS).  Stage2 writes the modules' base
segments into `mod_info_t`; `target_early_init()` reads it and
calls `patch_vfs_fptrs()` ([target_pcxt.c](../../src/target/pcxt/kernel/core/target_pcxt.c))
which:

1. Reads the VFS module header at offset 0 in the VFS code segment
   to find each VFS function's offset within VFS CS.
2. Patches `vfs_fptrs[i] = (vfs_offset_i, vfs_seg)` so core's
   ljmp through that table reaches the VFS entry stub.
3. Patches `core_fptrs[i] = (&core_<name>_entry, core_seg)` (one
   `PATCH_CORE` line per `mod_core.inc` entry, indices must
   match) so VFS's ljmp through that table reaches the core entry
   stub.

**Index/order discipline:** the indices in `PATCH_CORE(idx, name)`
must match the order in [`mod_core.inc`](../../src/kernel/common/mod/mod_core.inc).
Adding or removing an entry requires renumbering both files.

#### ia16-elf-gcc behaviour

- The compiler uses **SS for all data access** (`%ss:` prefix on
  every memory operand).  DS is a scratch register.
- **SS must always = 0** in kernel C code.  This is the load-bearing
  invariant that lets `(uintptr_t)&kernel_global` numerically equal
  the absolute linear address of the global.
- Boot ([`boot.S`](../../src/arch/i16/boot/boot.S)) sets SS=0.
- Every IRQ / syscall entry path
  ([`switch.S`](../../src/arch/i16/kernel/core/switch.S),
  [`trap.S`](../../src/arch/i16/kernel/core/trap.S)) checks the
  interrupted SS and switches to SS=0 before calling any C code.
- Far-call stubs do not touch SS — they assume the invariant.
- Assembly that needs to read kernel data must use the `%ss:`
  prefix.

#### Two-pass link for shared data

Core is linked first at SS=0 addresses (0x0600+).
[`gen_core_exports.sh`](../../scripts/gen_vfs_offsets.sh) extracts
core data/BSS symbol addresses into `core_exports.ld`.  VFS links
second with this symbol file so cross-module references like
`proc_table`, `current_core`, and `mod_core` resolve to the same
SS=0 addresses on both sides.  Both core and VFS link with strict
symbol resolution — any cross-module boundary violation is a
**compile-time error**.

### Directory Structure

See [Source Tree Structure](../getting_started/source_tree.md) for the
full layout.  Module-related files:

```
src/kernel/
  common/
    mod/
      module.h          ← module system macros (MOD_FUNC etc.)
      mod_core.h/.inc   ← core module interface (18 functions)
      mod_vfs.h/.inc    ← VFS module interface (39 functions)
    core/               ← shared data-only headers
      mem_layout.h      ← memory class enums, proc_image_segment_t
      page_types.h      ← page_id_t, PAGE_SIZE, page_count, oom_count
      proc_info.h       ← pcb_t struct, proc_state_t, proc_table, current
      sched_info.h      ← cpu tick counters
      subsys_info.h     ← subsystem name constants
    mem_region_kbuf.h   ← kernel-buffer → (page, off) inline helper
    config.h            ← build config, memory map constants
    spinlock.h          ← SMP spinlock / core_id()
  core/                 ← core module implementation
    core.c              ← mod_core struct initializer (default)
  vfs/                  ← VFS module (VFS, fd, tty, pipe, all FS)
    driver/             ← device drivers (blkdev, uart, spi, i2c, lcd, ...)

src/arch/<arch>/kernel/
  common/
    irq.h               ← arch_irq_save/restore, arch_preempt_disable/enable
    ioregs.h            ← I/O register definitions (+ CSR defs on riscv/xtensa)

src/target/pcxt/kernel/core/
  core.c                ← mod_core struct (i16 override, VFS-side far-call stubs)

src/target/pcxt/kernel/common/stubs/
  core_stubs.S          ← VFS-side caller stubs for core (auto-gen from .inc)
  core_entries.S        ← core-side target entry stubs (auto-gen from .inc)
  vfs_stubs.S           ← core-side caller stubs for VFS (auto-gen from .inc)
  vfs_entries.S         ← VFS-side target entry stubs (auto-gen from .inc)
  vfs_header.S          ← VFS module header (entry point table)
  vfs_mod_init.c        ← core-side mod_vfs struct (i16 only)
```

### Boundary Rules

Two strict rules:

1. **Inward rule:** Files outside `vfs/` MUST NOT include headers
   inside `vfs/` (e.g. `vfs/vfs.h`).  They use `mod/mod_vfs.h`.

2. **Outward rule:** Files inside `vfs/` MUST NOT include headers
   inside other module directories (e.g. `exec/exec.h`, `proc/proc.h`).
   They use `mod/mod_core.h` for core services and `common/core/*.h`
   for data-only type definitions (pcb_t, page_id_t, etc.).

**Allowed includes from any module:**
- `mod/mod_*.h` — public module interfaces
- `common/*` or standard headers — shared utilities and data types
- `arch/<arch>/kernel/common/*` — arch-specific IRQ/IO definitions
- Headers within the same module directory

**Enforcement:**
- On i16, the linker catches cross-module symbol references at build time.
- On all targets, `scripts/check_module_boundaries.sh` runs as a
  pre-build step in `build.sh` and fails the build if any VFS file
  includes `kernel/core/` headers or vice versa.

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
- ~~**blkdev page-indexed API**~~ — done
  ([issue #48](https://github.com/toyoshim-i/PiPAPo/issues/48),
  mem_region_wrapup Phase 1).
- **Source tree alignment** — restructure `src/kernel/` so each
  module's directory matches its boundary exactly (e.g. move
  `fd/`, `fs/` into `vfs/`; move `proc/`, `syscall/`, `mm/` into
  `core/`).  Currently the boundary is defined by the build
  system and `.inc` files, not by directory structure
