# Porting Third-Party Applications to PPAP

Guide for porting existing UNIX applications to PPAP.

---

## General Porting Pattern

PPAP uses a consistent pattern for all third-party code:

```
third_party/
  <app>/              git submodule (upstream source, never modified)
  patches/<app>/      PPAP-specific headers, patches, and config fragments
  patches/musl/       musl overlay + PIE linker scripts (shared by all musl apps)
  build_<app>.sh      standalone build script
```

### Key Principles

1. **Never modify upstream source** — use git submodules at specific tags
2. **Inject headers via `-isystem`** — override system headers without patching
3. **Standalone build scripts** — reproducible, callable from CMake
4. **CMake integration** — `add_custom_command` triggers the build script, stamps prevent rebuilds
5. **Multi-architecture** — build scripts should support all target architectures

## Target Constraints

Constraints vary by architecture. The common limits are:

| Constraint | All Targets | Notes |
|---|---|---|
| Data+BSS per process | 128 KB | SRAM/RAM pages allocated by ELF loader |
| Stack | 4 KB | 1 page |
| libc | musl 1.2.5 | Statically linked, PPAP syscall interface |
| Division | Software on ARM | m68k has hardware divide |

### Architecture-Specific Constraints

| | ARM (Thumb-1) | m68k |
|---|---|---|
| ISA | ARMv6-M, no Thumb-2 | Motorola 68000 |
| Code location | Flash (XIP, unlimited) | RAM (from page pool) |
| PIC model | `-fPIC -msingle-pic-base -mpic-register=r9` | `-fPIC -msep-data` |
| Compiler | `arm-none-eabi-gcc` | `m68k-elf-gcc` |

## Architecture Porting Contract

New CPU ports must satisfy the kernel continuation contract before they can run
general userland reliably.  PPAP allows a process to block while it is already
inside the kernel, so `sched_switch()` must preserve both the user resume point
and the in-flight kernel call chain that called it.

An architecture port must provide:

1. A per-process kernel stack, or an equivalent saved kernel-continuation
   frame.
2. A saved context pointer in `pcb_t.sp`.
3. A saved `pcb_t.kernel_sp` value when the architecture has a separate live
   kernel-stack top.
4. An `arch_sched_switch()` path that performs a real cooperative switch for
   blocking kernel code, not only a deferred preemption request.
5. A trap or return path that can restore either a normal user context or a
   suspended kernel continuation.

Native interrupt stacks are allowed as an optimization, but they are not the
continuation mechanism by themselves.  Timer, device, and fault handlers may
run on a hardware-preferred stack, while blocking syscalls and kernel waits must
still suspend on the process continuation stack or equivalent saved frame.

The shared kernel should not depend on whether the architecture uses hardware
stack switching, `mscratch`, USP/SSP, register windows, or real-mode far-call
frames.  Keep those details behind the architecture switch and trap helpers.
Restartable syscalls are also separate from continuation blocking: only
explicitly replay-safe syscalls should use the restart path.

See [`../kernel/context_switch.md`](../kernel/context_switch.md),
[`../kernel/stack.md`](../kernel/stack.md), and
[`../proposals/context_switch_cleanup.md`](../proposals/context_switch_cleanup.md)
for the current architecture-by-architecture model.

## Compiler Flags

### ARM

```sh
CFLAGS="-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft -Os"
CFLAGS="$CFLAGS -nostdinc -isystem $MUSL_SYSROOT/include -isystem $GCC_INCLUDE"
CFLAGS="$CFLAGS -fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative"
CFLAGS="$CFLAGS -ffunction-sections -fdata-sections"
```

Link with `-pie` to emit `R_ARM_RELATIVE` relocations.

### m68k

```sh
CFLAGS="-m68000 -Os"
CFLAGS="$CFLAGS -nostdinc -isystem $MUSL_SYSROOT/include -isystem $GCC_INCLUDE"
CFLAGS="$CFLAGS -fPIC -msep-data"
CFLAGS="$CFLAGS -ffunction-sections -fdata-sections"
```

Link with `-pie` to emit `R_68K_RELATIVE` relocations.

## musl libc

musl is cross-compiled for each architecture with PPAP's SVC/TRAP-based syscall interface.
Build: `third_party/build_musl.sh` → produces `build/<arch>/musl-sysroot/`.

### Syscall Remapping

musl internally uses Linux `*64` syscall variants (e.g., `stat64` not `stat`,
`fstat64` not `fstat`). The kernel's syscall table maps these numbers.
PPAP uses a unified 16-bit grouped numbering scheme shared across all architectures.

Key structs that must match musl's expectations:
- `struct stat` — Linux-compatible layout
- `struct dirent64` — variable-length with `d_ino`, `d_off`, `d_reclen`, `d_type`, `d_name`

## Rogue 5.4.4

Example port demonstrating the full pattern.
See `docs/archive/history/port-rogue.md` for the detailed audit.

### Curses Shim

Rogue requires curses — PPAP provides a minimal shim (~800 lines) in
`third_party/patches/rogue/curses.c`:

- Translates curses calls to VT100/ANSI escape sequences
- Diff-based `wrefresh()` — only emits changed cells
- `initscr()` queries terminal size via `ioctl(TIOCGWINSZ)`
- Arrow key parsing with escape sequence timeout
- Output buffering (4 KB) for efficient writes

### Header Overrides

| File | Purpose |
|---|---|
| `config.h` | Autoconf-style feature flags for PPAP |
| `curses.h` | Minimal curses API (WINDOW, chtype, key codes) |
| `pwd.h` | Stub returning fixed values (single-user) |

### Memory Budget (ARM)

| Segment | Size | Location |
|---|---|---|
| .text | 139 KB | Flash (XIP) |
| .data + .bss | 75 KB | SRAM |
| **Total SRAM** | **75 KB** | Within 128 KB limit |

On m68k, .text is also in RAM, but the 16 MB QEMU target has ample space.

## Porting Checklist

1. Add upstream source as git submodule: `git submodule add <url> third_party/<app>`
2. Create `third_party/patches/<app>/` with any header overrides
3. Audit memory usage: `.data` + `.bss` must fit in 128 KB
4. Identify and stub unsupported features (e.g., networking, fork, mmap)
5. Write `third_party/build_<app>.sh` with cross-compilation flags
6. Support all target architectures in the build script (or document which are supported)
7. Add CMake integration: custom command + stamp file + romfs install
8. Test on QEMU first (both ARM and m68k if supported), then hardware

## Related Documentation

- [userland_dev_guide.md](/docs/user/userland_dev_guide.md) — User-space programming guide
- [syscall.md](/docs/kernel/syscall.md) — System call reference
- [kernel.md](/docs/kernel/overview.md) — Kernel internals (ELF loader, PIE model)
- [archive/history/port-rogue.md](/docs/archive/history/port-rogue.md) — Rogue 5.4.4 porting details
