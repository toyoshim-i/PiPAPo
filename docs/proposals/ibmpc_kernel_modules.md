# Proposal: IBM PC Kernel Module Architecture

## Problem

The PPAP kernel compiled for ARM (Thumb2) is ~177 KB of code. On i8086
real mode, instructions are 30-50% larger, yielding an estimated 230-260 KB.
The 8086 segment limit is 64 KB per segment.

Compiling the full shared kernel with `__far` pointers everywhere would
pollute the portable codebase. Building a separate mini-kernel for i16
would create a divergent fork that's hard to maintain.

## Proposed Design: Segmented Kernel Modules

Split the kernel into independently-linked 64 KB modules, each in its
own code segment. Modules communicate through **far-call jump tables**
at fixed offsets — the only place segment awareness is needed.

### Memory Layout

```
Linear addr   Segment   Contents
─────────────────────────────────────────────────
0x00000       0x0000    IVT (1 KB) + BIOS data
0x03000       0x0300    Core kernel (~32 KB)
                          scheduler, page alloc, timer, console
                          syscall dispatch, IVT management
0x10000       0x1000    Module: VFS + filesystems (~40 KB)
                          vfs, namei, tmpfs, romfs, ufs, devfs,
                          procfs, fstab, fd, tty, pipe
0x20000       0x2000    Module: exec + process (~20 KB)
                          exec, flat/COM loader, proc management,
                          signal delivery
0x30000       0x3000    Module: subsystems (~20 KB, optional)
                          eCPU (Z80/8080/m68k), CP/M bridge,
                          DOS bridge, Human68k bridge
0x40000+      0x4000+   Page pool (~384 KB, up to 0x9FBFF)
```

Each module is ≤64 KB, compiled with `-mcmodel=small` (near pointers
within the module). No `__far` annotations in source code.

### Jump Table Interface

Each module exposes a jump table at offset 0x0000 of its segment:

```nasm
; Module VFS jump table at 0x1000:0x0000
vfs_jtab:
  jmp vfs_init          ; entry 0: near jump to vfs_init()
  jmp vfs_open          ; entry 1
  jmp vfs_close         ; entry 2
  jmp vfs_read          ; entry 3
  jmp vfs_write         ; entry 4
  ...
```

The core kernel calls into a module via a far call to the jump table:

```nasm
; Core kernel calling vfs_open (entry 1 in VFS module)
  lcall $0x1000, $3     ; far call to VFS segment, offset 3 (entry 1)
```

Or via a C wrapper in the core kernel:

```c
/* core/mod_vfs.h — VFS module call stubs */
#define VFS_SEG     0x1000u
#define VFS_INIT    0   /* jump table offsets */
#define VFS_OPEN    3

static inline int vfs_open(const char *path, int flags) {
    int ret;
    /* Far call: push args, lcall VFS_SEG:VFS_OPEN, get retval */
    __asm__ volatile (
        "pushw %2\n\t"
        "pushw %1\n\t"
        "lcall $0x1000, $3\n\t"
        "addw $4, %%sp"
        : "=a"(ret)
        : "r"(path), "r"((uint16_t)flags)
        : "memory"
    );
    return ret;
}
```

### Module Build Process

Each module is built as a separate flat binary:

```
ia16-elf-gcc -march=i8086 -Os -ffreestanding -nostdlib \
    -T module_vfs.ld -o module_vfs.elf \
    jtab_vfs.S vfs.c namei.c tmpfs.c romfs.c ...
ia16-elf-objcopy -O binary module_vfs.elf module_vfs.bin
```

The linker script for each module sets `. = 0x0000` (code starts at
offset 0 within its segment). The jump table assembly file is always
first so it occupies offset 0.

`mkpcimg.sh` places each module binary at the correct linear address
in the floppy image. Stage2 loads all modules (or the core kernel
loads them on demand from the floppy UFS).

### Calling Convention

- **Intra-module**: Normal near calls (ia16-elf-gcc default ABI).
  No changes to kernel source code.
- **Inter-module**: Far calls via jump tables. Only the thin wrapper
  layer (`mod_*.h`) uses far calls. The actual kernel C code is
  unaware of segments.
- **Arguments**: Passed on the stack (ia16 cdecl convention).
  Pointer arguments are near (16-bit offset within the caller's DS).
- **DS/ES handling**: Each module assumes DS=its own data segment.
  The jump table entry stub may need to switch DS before calling
  the module's C code (if caller DS ≠ module DS).

### Data Segment Sharing

**Option A: Shared DS (simpler)**
All modules share DS=0x0300 (core kernel data segment). Each module's
`.data`/`.bss` is placed in the core's data segment. Only `.text` is
in the module's own segment. This means:
- Pointer arguments work across modules (same DS)
- Global variables are shared naturally
- Data must fit in 64 KB total (across all modules)

**Option B: Per-module DS (more scalable)**
Each module has its own DS. Jump table stubs save/restore DS. Pointer
arguments need segment:offset conversion at module boundaries.
More complex but allows more data.

**Recommendation: Option A** for now. The kernel's total data+BSS on
ARM is ~35 KB, which fits in 64 KB even with 16-bit overhead.

### Impact on Shared Kernel Source

**None.** The shared kernel .c files compile unchanged for each module.
The segmentation is handled entirely by:
1. Linker scripts (one per module, setting text address)
2. Jump table assembly files (one per module, ~20 lines)
3. Far-call wrappers in the core kernel headers (~10 lines per API)
4. `mkpcimg.sh` / CMakeLists.txt build orchestration

Other architectures (ARM, m68k, RISC-V) continue to link everything
into a single binary as before.

### Phased Implementation

**P-3b-1**: Core kernel module only (scheduler, page alloc, console).
No VFS, no exec. Boots to idle loop. Proves the module build works.

**P-3b-2**: Add VFS module. Mount romfs or UFS from floppy.
Far-call wrappers for vfs_init/open/read/write/close.

**P-3b-3**: Add exec module. Load flat .COM binaries from filesystem.
Far-call wrappers for do_execve, proc_create, etc.

**P-3b-4**: Add subsystems module (optional). eCPU emulators,
DOS/CP/M bridges.

### Alternatives Considered

1. **Compact model (-mcmodel=medium)**: ia16-elf-gcc generates far
   calls automatically. But this requires recompiling ALL kernel code
   with far function pointers, which changes function pointer size
   from 2 to 4 bytes and affects vtables, callback signatures, etc.
   Too invasive.

2. **Huge model**: All pointers far (32-bit). Very slow, very invasive.

3. **Separate mini-kernel**: Write an i16-specific kernel from scratch.
   Avoids segment issues but creates a maintenance fork. The module
   approach avoids this by reusing the same .c files.

4. **Protected mode (286+)**: Switch to 16-bit protected mode for
   larger segments. But the proposal targets 8086 real mode (V30),
   and protected mode loses BIOS access.

### Open Questions

- Can ia16-elf-gcc handle the jump table assembly correctly, or do
  we need NASM for the inter-segment glue?
- Should the core kernel load modules from floppy at runtime (like
  a real module loader), or should stage2 load everything at fixed
  addresses (simpler)?
- How to handle function pointers that cross module boundaries (e.g.
  VFS ops callbacks)? These would need to be far pointers even in
  Option A. May need a small indirection layer.
