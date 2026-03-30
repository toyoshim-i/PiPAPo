# Proposal: PPAP Kernel Module System

## Problem

The PPAP kernel is growing (~177 KB code on ARM). On i8086 real mode,
the 64 KB segment limit makes it impossible to link as a single binary.
But adding i16-specific `__far` annotations would pollute the portable
kernel source.

## Proposed Design: Platform-Agnostic Module System

Introduce a kernel module system across **all platforms**, not just i16.
Modules define clear API surfaces. On 32-bit platforms, module calls are
direct function calls (zero overhead). On i16, the same API surfaces
become far-call boundaries.

This is **not** a microkernel — no message passing, no server processes.
It's a single-process plugin system with explicit API surfaces, like
Linux kernel modules but simpler.

### Module Definition

Each subsystem declares its API surface in a header:

```c
/* kernel/mod/mod_vfs.h */
#include "module.h"

MOD_DECLARE(vfs,
    int (*init)(void);
    int (*open)(const char *path, int flags);
    int (*close)(int fd);
    ssize_t (*read)(int fd, void *buf, size_t n);
    ssize_t (*write)(int fd, const void *buf, size_t n);
    /* ... */
)
```

The `MOD_DECLARE` macro generates:
- On 32-bit: a struct of function pointers, initialized to the real
  implementations. `mod_vfs.init()` compiles to an indirect call through
  the struct — practically free with branch prediction.
- On i16: a jump table in the module's text segment + far-call wrappers
  in the caller's segment.

### Module Registration

```c
/* kernel/fs/vfs.c */
#include "mod/mod_vfs.h"

MOD_DEFINE(vfs,
    .init  = vfs_init,
    .open  = vfs_open,
    .close = vfs_close,
    .read  = vfs_read,
    .write = vfs_write,
)
```

### Calling a Module

```c
/* kernel/syscall/sys_io.c */
#include "mod/mod_vfs.h"

long sys_read(int fd, char *buf, size_t n) {
    return mod_vfs.read(fd, buf, n);
}
```

On ARM/m68k/RISC-V: `mod_vfs.read` is a function pointer in a
statically-initialized struct → compiles to `ldr rN, [struct]; blx rN`.
Same as today's virtual dispatch for vfs_ops_t.

On i16: `mod_vfs.read` expands to a far-call wrapper that does
`lcall $seg, $offset` to the module's jump table entry.

### Platform Behavior

| Platform | MOD_DECLARE | MOD_DEFINE | Call overhead |
|----------|-------------|------------|---------------|
| ARM, m68k, RISC-V, Xtensa | struct of fn pointers | static init | ~1 indirect call |
| i16 | struct of fn pointers | runtime init (stubs) | ~2 indirect calls |

### The Core Module

The **core module** (`mod_core`) provides common services that all
other modules depend on: logging, memory allocation, and slab allocator.

```
  ┌─────────────────────────────────────────────────┐
  │              mod_core (core module)              │
  │  klog, kmem_alloc, kmem_free, page_alloc, ...   │
  │                                                  │
  │  mod_vfs.init() ──►  mod_exec.execve() ──►      │
  └──────────┬──────────────┬───────────────────────┘
             │mod_vfs.*     │mod_exec.*
       ┌─────▼──────┐ ┌────▼───────┐
       │  mod_vfs    │ │  mod_exec  │
       │  vfs, namei │ │  exec,     │
       │  fs drivers │ │  loaders   │
       │             │ │            │
       │ mod_core.*  │ │ mod_core.* │
       └─────────────┘ └────────────┘
```

ALL cross-module calls go through `mod_*` interfaces.  No direct
function calls across module boundaries.  On architectures that
need code-segment separation (e.g. i16), the `mod_*` struct entries
point to assembly stubs that handle the segment transition.  On
other architectures, they point directly to the real functions.

Inline functions (spinlock, string) compile into each module's code
and do not cross module boundaries.

### Architecture-Specific Segment Details

The mechanism for cross-segment calls varies by architecture.
See the target-specific documentation for details:

- **i16 (IBM PC)**: Two-level stub pattern — see `docs/proposals/pc_port.md` §9.5
- **Other architectures**: Not applicable (flat address space, no segments)

### mod_core API Surface

The core module exports common services used by all other modules.
The actual function list is defined in `src/kernel/common/mod/mod_core.h`.
Current exports:

```c
MOD_DECLARE_BEGIN(core)
  /* Logging */
  MOD_FUNC(core, void, klog, const char *)
  MOD_FUNC(core, void, klogf, const char *, ...)

  /* Slab allocator */
  MOD_FUNC(core, void, kmem_pool_init, kmem_pool_t *, void *, size_t, uint32_t)
  MOD_FUNC(core, void *, kmem_alloc, kmem_pool_t *)
  MOD_FUNC(core, void, kmem_free, kmem_pool_t *, void *)
  MOD_FUNC(core, uint32_t, kmem_free_count, const kmem_pool_t *)

  /* Memory region allocator (needed by tmpfs, procfs, exec, subsys) */
  MOD_FUNC(core, int, mem_region_alloc, proc_image_segment_t *,
                       ppap_mem_class_t, uint32_t, uint32_t)
  MOD_FUNC(core, void, mem_region_free, const proc_image_segment_t *)
  MOD_FUNC(core, uint32_t, mem_region_total_bytes, ppap_mem_class_t)
  MOD_FUNC(core, uint32_t, mem_region_free_bytes, ppap_mem_class_t)
MOD_DECLARE_END(core)
```

**Note on mem_region vs kmem:** These are complementary allocators:
- `kmem` — sub-page slab allocator for fixed-size kernel objects
  (vnodes ~138 B, files ~64 B).  Packs many objects into a static
  buffer.  O(1) alloc/free via intrusive free-list.
- `mem_region` — page-granularity allocator for process images and
  tmpfs data.  Abstracts per-arch backends (page pool on ARM/m68k,
  arenas on Xtensa).  4 KB minimum allocation.

Both are needed: `mem_region_alloc` for a 138-byte vnode would waste
97% of a 4 KB page.  `kmem` cannot allocate variable-size regions.

**mem_region callers:**
- `tmpfs.c` — allocating data pages for file contents (via mod_core ✓)
- `procfs.c` — /proc/meminfo stats (via mod_core ✓)
- `exec/*.c` loaders — allocating process image segments (direct, pending)
- `ecpu_*.c` — allocating emulator workspaces (direct, pending)

Functions only used within core (`mem_region_init`, `mem_region_alloc_at`,
`mem_region_largest_free_bytes`) are not exported.

Inline functions (string, spinlock, arch_irq) compile into each
module's code directly — they are NOT part of any module interface.
On IBM PC, the VFS module is loaded at linear 0x10000 (CS=0x1000).
No 64 KB alignment needed — a module only needs to fit in 64 KB
from its own start.

Stage2 loads each module binary and records its segment value in
a `mod_info_t` block at 0x0500.  The core reads this at boot and
patches the far-call wrapper tables.

### Shared Data Segment, Separate Code Segments (i16)

On i16, all modules share **DS=0** for data access.  Each module's
code (.text) lives in its own segment (separate CS).  Cross-module
calls use `lcall`/`lret` to switch CS; DS stays unchanged.

This means:
- Near data pointers work across modules (same DS=0)
- Pointer arguments passed to module functions work without
  serialization or copying — same DS, same addresses
- Global variables (proc_table, etc.) are accessible from all modules
- Each module's code must fit in 64 KB (its own CS)
- Total data+BSS from all modules must fit in 64 KB (shared DS=0)

Each module is compiled as a separate small-model binary.  The code
section starts at offset 0 in its own segment.  The data/BSS sections
are linked into the shared DS=0 address space at non-overlapping
addresses (coordinated via the core linker script reserving space).

The stub pattern only switches CS (no DS manipulation):

```asm
; Caller-side stub (in caller's segment):
vfs_init_caller_stub:
    lcall *[vfs_init_fptr]      ; far call to VFS CS
    ret                         ; near return to caller

; Target-side stub (in VFS segment):
vfs_init_entry:
    call vfs_init               ; near call within VFS
    lret                        ; far return to caller CS
```

**ia16-elf-gcc segment behaviour:**
- The compiler uses **SS for all data access** (`%ss:` prefix).
  DS is a scratch register (compiler stores arbitrary values in it).
- SS must always = 0 (the shared data segment invariant).
- Assembly stubs must use `%ss:` for any data access.

**Function pointer constraint:**
Near function pointers are CS-relative.  Passing a function pointer
from one module to another (e.g. `blkdev_t.read`) breaks because
the callee's CS differs from the module that set the pointer.
Such callbacks must go through `mod_core` or use far pointers.

**Why not medium model?**
- ia16-elf-ld 2.39 is broken: R_386_16 overflow for fartext
  sections, linker segfault with --noinhibit-exec

### Segment Manager

A **segment manager** in the core module tracks each module's code
segment base.  Stage2 loads module binaries into memory and records
their load addresses.  The core reads this info at boot and registers
each module's segment base.

```c
seg_register(MOD_ID_VFS,  vfs_segment);
seg_register(MOD_ID_EXEC, exec_segment);
```

Far-call stubs look up segment values from the manager table at
runtime — no hardcoded segment values in the generated code.

### Page-Indexed Memory Model

The 8086 address space is 1 MB, but near pointers are 16-bit (64 KB).
`proc_image_segment_t.base` is `void *` — on i16 that's 16-bit,
unable to address process segments beyond 64 KB.

Solution: `mm_page_alloc` returns a `page_id_t` (index, not pointer).
Cross-segment access uses `mm_page_read/write` which internally
sets up segment:offset pairs.  On 32-bit, `mm_page_to_ptr` provides
direct pointer access.  See `docs/proposals/pc_port.md` §9.5 for
the full API.

Modules that want i16 support must use `mm_page_read/write` instead
of raw pointer dereferences for cross-page data.  Components that
don't (eCPU emulators, subsystem bridges, tmpfs, procfs) are
excluded from the i16 build.

### Directory Structure and Visibility

Each module owns a subdirectory under `src/kernel/`.  Communication
between modules is **only** through `src/kernel/mod/mod_<name>.h`.

```
src/kernel/
  mod/
    module.h        ← module system macros (MOD_FUNC etc.)
    mod_vfs.h       ← public VFS interface (11 functions)
    mod_exec.h      ← public exec interface (future)
    mod_signal.h    ← public signal interface (future)
  vfs/
    vfs.h           ← PRIVATE: types + internal declarations
    vfs.c           ← function definitions + MOD_DEFINE
    namei.c         ← internal, includes vfs.h directly
  exec/
    exec.h          ← PRIVATE
    exec.c          ← ...
  common/           ← shared utilities (errno.h, string.h, stdint.h)
```

### Boundary Rules

Two strict rules, enforced by a compile-time validator:

1. **Inward rule:** Files outside `vfs/` MUST NOT include headers
   inside `vfs/` (e.g. `vfs/vfs.h`).  They use `mod/mod_vfs.h`.

2. **Outward rule:** Files inside `vfs/` MUST NOT include headers
   inside other module directories (e.g. `exec/exec.h`).  They use
   `mod/mod_exec.h`.

**Allowed includes from any module:**
- `mod/mod_*.h` — public module interfaces
- `common/*` or standard headers — shared utilities
- Headers within the same module directory

This makes each module self-contained.  Dependencies between modules
are explicit and documented in the `mod/mod_*.h` headers.

### Module Header Pattern

`mod/mod_<name>.h` uses `MOD_FUNC` macros to declare the API surface.
The module .c file has a separate `MOD_DEFINE` section that wires
functions into the struct.  No self-include tricks — the function
list appears in `mod_<name>.h` (declaration) and the .c file
(definition) separately.

```c
// mod/mod_vfs.h — declaration
MOD_DECLARE_BEGIN(vfs)
  MOD_FUNC(vfs, void, init, void)
  MOD_FUNC(vfs, int,  mount, const char *, ...)
MOD_DECLARE_END(vfs)

// vfs/vfs.c — definition (at bottom)
#include "../mod/mod_vfs.h"
MOD_DEFINE_BEGIN(vfs)
  MOD_IMPL(vfs, init)
  MOD_IMPL(vfs, mount)
MOD_DEFINE_END()
```

All platforms use `mod_vfs.init()` — uniform syntax.
On 32-bit, this is a direct function pointer call.
On i16, the struct entry points to a near stub that does `lcall` to
the target module's code segment.

When i16 exceeds 64 KB, the i16 MOD_FUNC generates far-call thunks
instead of plain extern declarations.  The thunk code lives in the
caller's segment and performs `lcall` to the target module's segment:

```nasm
; Auto-generated thunk for vfs_init (caller segment)
_vfs_init_thunk:
    lcall $VFS_SEG, $vfs_init_offset
    ret
```

### Enforcement Script

```bash
#!/bin/bash
# scripts/check_module_boundaries.sh
# Validates that module directories only communicate via mod/ headers.

MODULES="vfs exec signal subsys proc mm fd fs blkdev cpu"
ERRORS=0

for mod in $MODULES; do
  dir="src/kernel/${mod}"
  [ -d "$dir" ] || continue

  # Rule 1: no external file includes headers from this module dir
  external=$(grep -rn "\"${mod}/\|\"\.\./${mod}/" src/kernel/ \
    --include="*.c" --include="*.h" \
    | grep -v "src/kernel/${mod}/" \
    | grep -v "src/kernel/mod/")
  if [ -n "$external" ]; then
    echo "BOUNDARY: files outside ${mod}/ include its internal headers:"
    echo "$external"
    ERRORS=$((ERRORS + 1))
  fi

  # Rule 2: files inside this module don't include other module internals
  for other in $MODULES; do
    [ "$other" = "$mod" ] && continue
    internal=$(grep -rn "\"${other}/\|\"\.\./${other}/" \
      "src/kernel/${mod}/" \
      --include="*.c" --include="*.h" 2>/dev/null)
    if [ -n "$internal" ]; then
      echo "BOUNDARY: ${mod}/ includes ${other}/ internal headers:"
      echo "$internal"
      ERRORS=$((ERRORS + 1))
    fi
  done
done

if [ $ERRORS -gt 0 ]; then
  echo "Found $ERRORS boundary violation(s)"
  exit 1
fi
echo "Module boundaries OK"
```

### Migration Plan

For each module (starting with VFS):

1. Create `mod/mod_<name>.h` with MOD_FUNC declarations ✓ (done for VFS)
2. Add MOD_DEFINE in the module's .c file
3. Migrate external callers: replace `#include "../vfs/vfs.h"` with
   `#include "../mod/mod_vfs.h"`, change `vfs_init()` to `mod_vfs.init()`
4. Move shared types (vnode_t, vfs_ops_t) into mod_vfs.h or a
   separate `mod/vfs_types.h` so external callers don't need vfs.h
5. Run boundary checker — zero violations
6. Repeat for next module

### Build System

**32-bit targets (ARM, m68k, RISC-V, Xtensa):**
Link everything into one binary.  The module struct is initialized
at compile time.  Module boundaries are enforced by the include
checker script, not by the linker.

**i16 target (IBM PC):**
Each module is a separate flat binary with its own linker script
and code segment.  All modules share DS=0 for data.  CMakeLists.txt
builds N+1 targets: core + one per module.  `mkpcimg.sh` packages
them into the UFS floppy.  See `docs/proposals/pc_port.md` §9.5
for the concrete memory layout and build steps.

### Current Status (as of 2026-03-30)

The module system is implemented and working on all platforms:

- Module macros (MOD_DECLARE/MOD_DEFINE) working
- mod_core (16 functions), mod_vfs (14 functions), mod_exec (1 function)
- Static asserts: `MOD_CORE_FUNC_COUNT` / `MOD_VFS_FUNC_COUNT` catch
  mismatches between struct and assembly stub tables at compile time
- `kernel/common/` directory for shared headers

On i16, the kernel is split into separate code segments (core ~30 KB +
VFS ~26 KB).  Stage2 loads both binaries from UFS floppy.  Core boots,
detects VFS module, and VFS data is placed at DS:0xA000.  Both call
directions work via two-level far-call stubs.

**Blocker:** The i16 build still uses `--unresolved-symbols=ignore-all`
because ~82 cross-module calls bypass the `mod_*` interfaces.
Unresolved symbols silently link to address 0, causing crashes at
runtime (e.g. `fd_close_all` in `sys_exit`, `sched_wakeup` in pipe.c).

### Boundary Violation Audit

**82 violations** across 11 files.  Grouped by direction:

#### Core → VFS (bypassing mod_vfs)

| Function | Defined in | Called from | Occurrences |
|----------|-----------|-------------|-------------|
| `fd_pool_init` | sys_fs.c | main.c | 1 |
| `file_alloc/free` | sys_fs.c | sys_fs.c, sys_proc.c | 5 |
| `fd_alloc/free/get` | fd.c | sys_fs.c, sys_proc.c | ~18 |
| `fd_close_all` | fd.c | sys_proc.c (sys_exit) | 1 |
| `fd_inherit` | fd.c | sys_proc.c (vfork) | 1 |
| `tty_get_dev` | tty.c | sys_fs.c | 1 |
| `tty_rx_notify` | tty.c | sched.c | 1 |
| `fstab_parse/mount_all` | fstab.c | main.c | 2 |
| direct `fd_table[]` | pcb_t | sys_fs/io/poll.c | ~30 |

#### VFS → Core (bypassing mod_core)

| Function/Variable | Defined in | Called from | Occurrences |
|----------|-----------|-------------|-------------|
| `sched_wakeup` | sched.c | pipe.c, tty.c | 7 |
| `sched_yield` | sched.c | pipe.c, tty.c | 8 |
| `svc_set_restart` | syscall.c | pipe.c, tty.c | 6 |
| `sched_get_ticks` | sched.c | procfs.c | 1 |
| `current_core` (data) | proc.c | pipe.c, tty.c, namei.c | ~6 |
| `proc_table` (data) | proc.c | tty.c, procfs.c | ~9 |
| `uart_putc/getc/rx_avail` | drivers/ | tty.c, devfs.c | 4 |

### Two-Pass Link for Shared Data

Both modules share DS=0, so global variables (`current_core`,
`proc_table`, `tick_count`, etc.) are accessible from either module
at the **same** linear address.  But each module is linked separately,
so the linker doesn't know the other module's symbol addresses.

**Solution: two-pass link.**

1. **Pass 1:** Link the core module → produce core ELF.
2. **Generate symbol file:** Extract shared data addresses from core
   ELF via `nm`, generate a linker symbol file (`core_exports.ld`):
   ```
   /* Auto-generated from ppap_ibmpc.elf */
   current_core = 0x8208;
   proc_table   = 0x84E6;
   tick_count   = 0x7A70;
   /* ... */
   ```
3. **Pass 2:** Link the VFS module with `core_exports.ld` as an
   additional linker script.  The VFS linker resolves `current_core`
   etc. to the correct DS=0 addresses.

This eliminates `--unresolved-symbols=ignore-all` for data references.
For function references, all cross-module calls must go through
`mod_core` / `mod_vfs` stubs.

The symbol file is regenerated automatically by CMake whenever the
core binary changes.

### Completion Plan

#### Step 1: Export remaining VFS→Core functions via mod_core

Add to `mod_core.h` (and corresponding stubs):

```c
MOD_FUNC(core, void, sched_wakeup, void *)
MOD_FUNC(core, void, sched_yield, void)
MOD_FUNC(core, void, svc_set_restart, void)
MOD_FUNC(core, uint32_t, sched_get_ticks, void)
MOD_FUNC(core, void, uart_putc, char)
MOD_FUNC(core, int, uart_getc, void)
MOD_FUNC(core, int, uart_rx_avail, void)
```

Migrate callers in `pipe.c`, `tty.c`, `devfs.c`, `procfs.c` to
`mod_core.sched_wakeup()` etc.

#### Step 2: Export remaining Core→VFS functions via mod_vfs

Add to `mod_vfs.h`:

```c
MOD_FUNC(vfs, void, fd_pool_init, void)
MOD_FUNC(vfs, void, tty_rx_notify, int)
MOD_FUNC(vfs, int, fstab_parse, void)
MOD_FUNC(vfs, int, fstab_mount_all, void)
```

Migrate callers in `main.c`, `sched.c`.

FD-related exports (`fd_close_all`, `fd_inherit`) are handled in
Step 3 as part of the fd table redesign.

#### Step 3: System-wide file descriptor table

Redesign the fd layer to separate concerns between core and VFS.

**Current problem:** `fd_table[]` lives in `pcb_t` (core BSS), but
all FD operations are in `fd.c` (VFS module), creating ~30 direct
cross-module accesses.  Passing `pcb_t *` to VFS also leaks process
internals into VFS.

**New design: core owns the mapping, VFS owns the file objects.**

VFS manages a **system-wide file descriptor pool**, indexed by an
opaque descriptor ID.  VFS is process-agnostic — it never sees pid
or pcb_t.

```
  Core (per-process)              VFS (system-wide, process-agnostic)
  ┌──────────────────┐            ┌──────────────────────────────┐
  │ pcb_t.fd_map[]   │            │ fd_pool[MAX_SYSTEM_FDS]      │
  │  [0] → desc 5   ─┼───────►   │  [5] vnode, offset, flags,   │
  │  [1] → desc 12  ─┼───────►   │      refcount                │
  │  [2] → -1       │            │  [12] ...                    │
  └──────────────────┘            └──────────────────────────────┘
```

- **Core** owns `fd_map[MAX_FDS]` in `pcb_t` — a small array of
  descriptor IDs (e.g. `int16_t[16]` = 32 bytes).  Core resolves
  per-process fd number → descriptor ID.
- **VFS** owns `fd_pool[MAX_SYSTEM_FDS]` (e.g. 128 entries) — each
  entry holds vnode pointer, offset, flags, and a reference count.
  VFS allocates/releases entries and performs I/O by descriptor ID.
- Syscalls: core looks up `fd_map[local_fd]` → `desc_id`, then
  calls `mod_vfs.fd_read(desc_id, buf, n)`.

**Operations:**

- `open`: core calls `mod_vfs.fd_open(path, flags)` → VFS allocates
  a pool entry, returns `desc_id` → core stores in `fd_map[]`.
- `read/write`: core resolves `fd_map[fd]` → `desc_id`, calls
  `mod_vfs.fd_read(desc_id, ...)`.
- `close`: core resolves `fd_map[fd]` → `desc_id`, clears the slot,
  calls `mod_vfs.fd_release(desc_id)` (decrements refcount, frees
  at zero).
- `exit (fd_close_all)`: core iterates its own `fd_map[]`, calls
  `mod_vfs.fd_release(desc_id)` for each valid entry.  VFS never
  needs to know about pid.
- `fork (fd_inherit)`: core copies parent's `fd_map[]` to child,
  calls `mod_vfs.fd_acquire(desc_id)` for each shared entry to bump
  refcount.
- `dup/dup2`: core assigns a new `fd_map[]` slot pointing to the
  same `desc_id`, calls `mod_vfs.fd_acquire(desc_id)`.

This is similar to real UNIX kernels (system-wide `struct file`
table + per-process fd table).  Benefits:
- VFS is fully process-agnostic — no pid, no pcb_t, no proc.h
- Fork/dup sharing is natural (refcounted pool entries)
- Most fd operations are VFS-internal; core only calls a small
  mod_vfs surface
- `fd_map[]` in pcb_t is tiny (just indices)

```c
/* mod_vfs.h exports for fd operations */
MOD_FUNC(vfs, int, fd_open, const char *, int)     /* → desc_id */
MOD_FUNC(vfs, int, fd_release, int)                /* desc_id */
MOD_FUNC(vfs, int, fd_acquire, int)                    /* desc_id */
MOD_FUNC(vfs, long, fd_read, int, void *, uint32_t)
MOD_FUNC(vfs, long, fd_write, int, const void *, uint32_t)
```

Move `sys_fs.c` and `sys_io.c` into the VFS module — file syscalls
are logically VFS operations.  This eliminates the bulk of Core→VFS
boundary violations.

#### Step 4: Two-pass link for shared data

Implement the `core_exports.ld` generation in CMake:
```cmake
add_custom_command(TARGET ppap_ibmpc POST_BUILD
  COMMAND ${CMAKE_NM} $<TARGET_FILE:ppap_ibmpc>
    | grep -E "B current_core|B proc_table|B tick_count|..."
    | awk '{ print $$3 " = 0x" $$1 ";" }'
    > ${CMAKE_CURRENT_BINARY_DIR}/core_exports.ld
)
```

Link VFS with the generated symbol file:
```cmake
target_link_options(ppap_ibmpc_vfs PRIVATE
  -T ${CMAKE_CURRENT_BINARY_DIR}/core_exports.ld
)
```

#### Step 5: Remove --unresolved-symbols=ignore-all

After Steps 1-4, all cross-module references are resolved:
- Functions: through `mod_core` / `mod_vfs` stubs
- Data: through `core_exports.ld` symbol file

Remove `--unresolved-symbols=ignore-all` from both link commands.
Any remaining unresolved symbol becomes a **build error**, catching
future boundary violations at compile time.

### Design Decisions

**Type safety:** `MOD_FUNC` generates struct fields with full type
signatures on all platforms.  The implementation's functions must
match exactly — any mismatch is a compile error.  Callers use
`mod_vfs.init()` syntax everywhere.

**Naming:** Struct fields are unprefixed (`init`, `mount`,
`vnode_alloc`).  Real function names are prefixed (`vfs_init`,
`vfs_mount`, `vfs_alloc_vnode`).  The `MOD_FUNC(vfs, void, init, ...)`
macro handles the mapping.

**UART in core:** Theoretically, uart is a character device driver
that belongs under VFS/devfs.  Pragmatically, `klog` needs `uart_putc`
before VFS is initialized — uart must be in core for bootstrap.
VFS accesses uart through `mod_core.uart_putc()` etc.

**Module count:** ~5 API surfaces:
- mod_core (klog, kmem, mem_region, sched, uart — 16 functions, grows as needed)
- mod_vfs (VFS, vnode, fd, tty, fstab — 14 functions, grows as needed)
- mod_exec (process creation + loading — 1 function currently)
- mod_signal (signal delivery — future)
- mod_subsys (eCPU + bridges — future)

**API surface minimization:** Module interfaces should expose the
smallest possible set of functions.  Start with what's needed, then
continuously reduce as the codebase evolves:
- Merge related functions (e.g. lookup + lookup_flags → one function
  with default flags)
- Move helpers inside the module (if only one external caller, maybe
  the caller belongs in the module)
- Prefer passing data over calling back (reduce round-trips)

This is an ongoing discipline, not a one-time design.  Every code
review should ask: "can this module boundary be narrower?"

**Loading:** Stage2 loads all modules at boot time at fixed addresses.
No runtime module loading for now — simpler and sufficient.
