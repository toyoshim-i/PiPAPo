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

Each module owns a subdirectory under `src/kernel/`.  Headers inside
a module directory are **private** — only source files within the same
directory may include them.  The public interface is exposed through
`src/kernel/mod/mod_<name>.h`.

```
src/kernel/
  mod/
    module.h        ← module system macros
    mod_vfs.h       ← public VFS interface (11 functions)
    mod_exec.h      ← public exec interface (future)
    mod_signal.h    ← public signal interface (future)
  vfs/
    vfs.h           ← PRIVATE: types + internal declarations
    vfs.c           ← MOD_STATIC functions + MOD_IMPLEMENTATION
    namei.c         ← internal, can include vfs.h directly
  exec/
    exec.h          ← PRIVATE
    exec.c          ← ...
  ...
```

**Rule:** Files outside `src/kernel/vfs/` MUST NOT `#include "vfs/vfs.h"`.
They include `mod/mod_vfs.h` instead.  This is enforced by a compile-time
validator (CMake custom target or pre-commit hook) that greps for
cross-boundary includes and fails the build.

**Enforcement mechanism:**

```bash
# scripts/check_module_boundaries.sh (run by CMake or pre-commit)
# Fails if any file outside vfs/ includes vfs/vfs.h
for mod in vfs exec signal; do
  violations=$(grep -rn "\"${mod}/${mod}.h\"" src/kernel/ \
    --include="*.c" --include="*.h" \
    | grep -v "src/kernel/${mod}/")
  if [ -n "$violations" ]; then
    echo "ERROR: cross-module include of ${mod}/${mod}.h:"
    echo "$violations"
    exit 1
  fi
done
```

**MOD_STATIC:** Module functions defined with `MOD_STATIC` are `static`
on 32-bit platforms (only accessible via the module struct) and extern
on i16 (direct calls needed).  This enforces the boundary at link time:
any direct call to a `MOD_STATIC` function from outside the module is
a compile error.

Migration order:
1. Move callers from `vfs.h` to `mod/mod_vfs.h`
2. Mark module functions `MOD_STATIC` in the .c file
3. Move internal-only declarations from `vfs.h` into the .c files
   or a `vfs_internal.h`
4. Add boundary checker to CMake / CI

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
functions (8 VFS + 3 vnode lifecycle).  Self-include pattern for
single-source declaration + definition.  MOD_STATIC defined.

**Phase 2 (P-4a.2):** Migrate VFS callers.
- Move external callers from `vfs/vfs.h` to `mod/mod_vfs.h`
- Mark module functions `MOD_STATIC` in vfs.c
- Add boundary enforcement script to CMake
- Move internal declarations to `vfs_internal.h`

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
