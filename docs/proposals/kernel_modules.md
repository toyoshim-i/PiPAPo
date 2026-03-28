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

**Why not isolated DS per module?**
- Pointer arguments would need serialization at every module call
  (copy-in/copy-out, like microkernel IPC) — the C compiler cannot
  access data via ES instead of DS
- Adds significant complexity and overhead for every cross-module call

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

### Current Status

The module system is implemented and working on all platforms:

- Module macros (MOD_DECLARE/MOD_DEFINE) working
- mod_vfs (11 functions), mod_core (10 functions), mod_exec (1 function)
- VFS callers migrated to `mod_vfs.init()` syntax on all platforms
- Boundary enforcement script validates no cross-module includes
- `kernel/common/` directory for shared headers

On i16, the kernel is split into separate code segments (core 28 KB +
VFS ~27 KB).  Stage2 loads both binaries from UFS floppy.  Core boots,
detects VFS module, and VFS data is placed at DS:0xA000.  Core→VFS far
calls work (vfs_init code executes in CS=0x1000).  VFS→core far calls
(e.g. mod_core.kmem_pool_init from within vfs_init) hang — suspected
stack frame mismatch from the far call's extra CS:IP push.
See `docs/proposals/pc_port.md` P-4b for the current blocker and plan.

**Known boundary violations to fix:**
- `exec/*.c` and `cpu/ecpu_*.c` include `mm/mem_region.h` directly —
  will be migrated to `mod_core.mem_region_*()` when exec/subsys
  become separate modules

### Phased Rollout

**P-4a:** ✓ Done.  Module macros, mod_vfs/mod_core/mod_exec headers,
VFS caller migration to `mod_vfs.init()`, boundary enforcement,
`kernel/common/` directory.

**P-4a.3:** ✓ Done.  Added `mem_region_alloc`/`mem_region_free` and
query functions to `mod_core.h` (10 functions total).  Migrated
`fs/tmpfs.c` and `fs/procfs.c` to `mod_core.mem_region_*()`.
Remaining: `exec/*.c` and `cpu/ecpu_*.c` still use `mem_region.h`
directly — will be migrated when exec/subsys become modules.

**P-4b (i16 only):** In progress.  Segment split — separate CS per
module, shared DS=0.  See `docs/proposals/pc_port.md` P-4b for
detailed done/remaining checklist.

**Future:** Add more module boundaries as needed:
- `mod/mod_signal.h`: signal delivery, sigreturn
- `mod/mod_subsys.h`: eCPU registration, bridge dispatch

### Design Decisions

**Type safety:** `MOD_FUNC` generates struct fields with full type
signatures on all platforms.  The implementation's functions must
match exactly — any mismatch is a compile error.  Callers use
`mod_vfs.init()` syntax everywhere.

**Naming:** Struct fields are unprefixed (`init`, `mount`,
`alloc_vnode`).  Real function names are prefixed (`vfs_init`,
`vfs_mount`, `vfs_alloc_vnode`).  The `MOD_FUNC(vfs, void, init, ...)`
macro handles the mapping.

**Module count:** ~5 API surfaces:
- mod_core (klog, kmem, mem_region — 10 functions currently, grows as needed)
- mod_vfs (VFS operations + vnode lifecycle — 11 functions)
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
