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
0x03000       Core kernel text (~16 KB)
0x07000       Module: VFS text (~24 KB)
0x0D000       Module: exec text (~12 KB)
0x10000       Module: subsystems (~16 KB, optional)
0x14000       Shared data segment (DS, all modules, ~20 KB)
0x19000       Page pool (up to 0x9FBFF)
```

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

### Impact on Existing Code

**Minimal.** The change is:
1. Add `mod/mod_*.h` headers with `MOD_DECLARE` for each subsystem
2. Add `MOD_DEFINE` in each subsystem's main .c file
3. Change cross-subsystem calls from `vfs_open(...)` to
   `mod_vfs.open(...)` (or keep the old names as macros)

On 32-bit platforms, `MOD_DECLARE` generates a struct of function
pointers. The indirect call is the same pattern already used for
`vfs_ops_t`, `cpu_ops`, etc. — no performance regression.

### Build System

**32-bit targets (ARM, m68k, RISC-V, Xtensa):**
Link everything into one binary as today. `MOD_DEFINE` creates a
static struct. No separate compilation needed.

**i16 target:**
Each module is compiled and linked as a separate flat binary with its
own linker script (`. = 0x0000`, module-internal addressing). The
`mkpcimg.sh` script places them at the right linear addresses.
CMakeLists.txt builds N+1 targets: core + one per module.

### Phased Rollout

**Phase 1 (P-3b-1):** Define MOD_DECLARE/MOD_DEFINE macros.
Convert VFS as the first module (it already has a vtable pattern).
Build core kernel for i16 without VFS — boots to idle.

**Phase 2 (P-3b-2):** Build VFS as a separate module on i16.
Far-call wrappers for vfs_init/open/read/write/close.
Mount romfs from floppy.

**Phase 3 (P-3b-3):** Convert exec subsystem to a module.
Load flat .COM binaries.

**Phase 4:** Convert remaining subsystems (eCPU, bridges) to modules.
Optional — only loaded if present on the floppy.

### Design Decisions

**Type safety:** On 32-bit platforms, `MOD_DECLARE` generates `static
inline` wrappers that provide compile-time type checking. On i16, the
wrappers are omitted (plain far-call macros) since ia16-elf-gcc 6.3.0
may not optimize them away. Type correctness is verified by the 32-bit
builds; i16 relies on those results.

**Module count:** ~4 API surfaces:
- mod_vfs (filesystem ops)
- mod_exec (process creation + loading)
- mod_signal (signal delivery)
- mod_subsys (eCPU + bridges)

**Loading:** Stage2 loads all modules at boot time at fixed addresses.
No runtime module loading for now — simpler and sufficient.
