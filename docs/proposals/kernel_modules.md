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
| i16 | far-call wrapper + jump table | jump table asm | ~4 extra instructions |

### i16 Memory Layout

On i16, modules are loaded at paragraph-aligned addresses. Unlike the
earlier proposal with fixed 64 KB segments, modules are **packed tightly**
with adjustable segment registers:

```
Linear addr   Contents
────────────────────────────────────────────
0x00600       Core kernel text (~42 KB)  ← current: all code here
0x0B200       Module: VFS + FS text (~15 KB, future)
0x0EE00       Module: exec text (~8 KB, future)
0x10E00       Module: subsystems (~10 KB, optional)
  ...         .data + .bss shared (DS=0, ~6 KB)
  ...         Page pool (up to 0x9FBFF)
```

Addresses are paragraph-aligned (16 bytes). Modules packed tightly —
no 64 KB alignment waste. Each module only needs to fit in 64 KB from
its own start address.

Each module's CS is set to its paragraph-aligned start address / 16.
For example, VFS at linear 0x07000 → CS=0x0700. No 64 KB alignment
needed, no wasted padding. A module only needs to fit in 64 KB from
its *own* start — overlapping with the next segment's range is fine
in real mode since segments are just base+offset windows.

Stage2 (or the core kernel at boot) loads each module binary and
records its segment value. The far-call wrappers use these values.

### Shared Data Segment

All modules share a single DS (e.g., 0x1400 in the layout above).
This means:
- Near data pointers work across modules (same DS)
- Global variables (proc_table, etc.) are accessible from all modules
- Struct pointers passed as arguments work without conversion
- Total data+BSS must fit in 64 KB (currently ~35 KB on ARM, fine)

Each module's `.data` and `.bss` are linked into the shared data
segment, not into the module's own code segment. The code segment
contains only `.text` and `.rodata`.

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

On 32-bit, callers use `mod_vfs.init()`.
On i16, callers use `vfs_init()` (direct call, same binary).

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

### Module Header Pattern

Each module header (`mod/mod_*.h`) serves as both declaration and
definition, controlled by `MOD_IMPLEMENTATION`:

```c
// Callers: just include
#include "mod/mod_vfs.h"

// Implementation: define flag, then include (same header)
#define MOD_IMPLEMENTATION
#include "mod/mod_vfs.h"
```

The header uses `_MOD_IMPL_PHASE` to self-include in implementation
mode, so the `MOD_FUNC` list is written exactly once.  Per-function
documentation is part of the module header.

### Build System

**32-bit targets (ARM, m68k, RISC-V, Xtensa):**
Link everything into one binary as today.  The module struct is
initialized at compile time.  `MOD_STATIC` functions are inlined
or optimized to direct calls by the compiler.

**i16 target:**
Currently all in one binary (46 KB).  When the kernel exceeds 64 KB,
each module is compiled and linked as a separate flat binary with its
own linker script.  `mkpcimg.sh` places them at paragraph-aligned
addresses.  CMakeLists.txt builds N+1 targets: core + one per module.

### Current Status

P-3b is complete: the full PPAP kernel (MM, proc, VFS, CPU, scheduler)
compiles and boots to idle on i16.  Binary size: 46 KB of the 63 KB
budget (13 KB headroom).

The module system is needed when adding:
- exec loader + flat binary support (~5-10 KB)
- signal delivery (~3-5 KB)
- additional filesystem features (~5 KB)

At ~58 KB, the module split becomes mandatory.

### Phased Rollout

**Phase 1 (P-4a):** ✓ Done.  module.h macros + mod_vfs.h with 11
functions (8 VFS + 3 vnode lifecycle).

**Phase 2 (P-4a.2):** Migrate VFS callers.
- Move external callers from `vfs/vfs.h` to `mod/mod_vfs.h`
- Change call sites: `vfs_init()` → `mod_vfs.init()` (32-bit)
- Move shared types into mod_vfs.h so callers don't need vfs.h
- Add MOD_DEFINE block in vfs.c (explicit, no self-include)
- Add boundary enforcement script

**Phase 3 (P-4b):** Add exec module with flat binary loader.
- `mod/mod_exec.h`: do_execve, proc_create, proc_free
- Exec functions become `MOD_STATIC`

**Phase 4 (P-4c):** Add signal + subsystems modules.
- `mod/mod_signal.h`: signal delivery, sigreturn
- `mod/mod_subsys.h`: eCPU registration, bridge dispatch

### Design Decisions

**Type safety:** On 32-bit, `MOD_FUNC` generates struct fields with
full type signatures.  The implementation's `MOD_STATIC` functions
must match exactly — any mismatch is a compile error.  On i16,
`MOD_FUNC` generates extern declarations with prefixed names.

**Naming:** Struct fields are unprefixed (`init`, `mount`,
`alloc_vnode`).  Real function names are prefixed (`vfs_init`,
`vfs_mount`, `vfs_alloc_vnode`).  The `MOD_FUNC(vfs, void, init, ...)`
macro handles the mapping.

**Module count:** ~4 API surfaces:
- mod_vfs (VFS operations + vnode lifecycle, 11 functions)
- mod_exec (process creation + loading)
- mod_signal (signal delivery)
- mod_subsys (eCPU + bridges)

**Loading:** Stage2 loads all modules at boot time at fixed addresses.
No runtime module loading for now — simpler and sufficient.
