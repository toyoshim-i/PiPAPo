# Porting Third-Party Applications to PPAP

Guide for porting existing UNIX applications to PPAP.

---

## General Porting Pattern

PPAP uses a consistent pattern for all third-party code:

```
third_party/
  <app>/              git submodule (upstream source, never modified)
  patches/<app>/      PPAP-specific headers and patches
  configs/            build configurations (defconfig, linker scripts)
  build-<app>.sh      standalone build script
```

### Key Principles

1. **Never modify upstream source** — use git submodules at specific tags
2. **Inject headers via `-isystem`** — override system headers without patching
3. **Standalone build scripts** — reproducible, callable from CMake
4. **CMake integration** — `add_custom_command` triggers the build script, stamps prevent rebuilds

## Target Constraints

| Constraint | Limit | Notes |
|---|---|---|
| ISA | ARMv6-M (Thumb-1) | No Thumb-2, no hardware division |
| Data+BSS per process | 128 KB | SRAM pages allocated by ELF loader |
| Code | Unlimited (XIP) | Runs directly from flash |
| libc | musl 1.2.5 | Statically linked, PPAP syscall interface |
| PIC model | `-fPIC -msingle-pic-base -mpic-register=r9` | GOT-based, r9 set by kernel |
| Division | Software only | libgcc provides `__aeabi_uidiv` etc. |

## Compiler Flags

```sh
CFLAGS="-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft -Os"
CFLAGS="$CFLAGS -nostdinc -isystem $MUSL_SYSROOT/include -isystem $GCC_INCLUDE"
CFLAGS="$CFLAGS -fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative"
CFLAGS="$CFLAGS -ffunction-sections -fdata-sections"
```

Link with `-pie` to emit `R_ARM_RELATIVE` relocations (patched by the ELF loader at load time).

## musl libc

musl is cross-compiled for ARMv6-M with PPAP's SVC-based syscall interface.
Build: `third_party/build-musl.sh` → produces `build/musl-sysroot/`.

### Syscall Remapping

musl internally uses Linux `*64` syscall variants (e.g., `stat64` not `stat`,
`fstat64` not `fstat`). The kernel's syscall table maps these numbers.
Legacy syscall numbers are dead — musl never emits them.

Key structs that must match musl's expectations:
- `struct stat` — 88-byte Linux-compatible layout (not the kernel's internal 16-byte version)
- `struct dirent64` — variable-length with `d_ino`, `d_off`, `d_reclen`, `d_type`, `d_name`

## busybox

Built via `third_party/build-busybox.sh` with a custom defconfig.

### Configuration

Two fragment files in `third_party/configs/`:
- `busybox_sh.fragment` — hush shell + init (CONFIG_HUSH=y)
- `busybox_ppap.fragment` — applet selection, static linking, musl sysroot

### Build Approach

1. Start from `allnoconfig`
2. Apply fragments via `scripts/kconfig/merge_config.sh`
3. Cross-compile with musl sysroot
4. Link with `-pie` and custom linker script (`configs/busybox.ld`)
5. Strip → install to romfs

### Split Binaries

To reduce per-process SRAM footprint (GOT entries), busybox is optionally
split into separate binaries:
- `init` — PID 1 only (80 GOT entries, minimal SRAM)
- `sh` — hush shell + applets (260 GOT entries)
- Full busybox — all applets (784 GOT entries)

## Rogue 5.4.4

Example port demonstrating the full pattern.
See `docs/history/port-rogue.md` for the detailed audit.

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

### Memory Budget

| Segment | Size | Location |
|---|---|---|
| .text | 139 KB | Flash (XIP) |
| .data + .bss | 75 KB | SRAM |
| **Total SRAM** | **75 KB** | Within 128 KB limit |

`xcrypt.c` is excluded (71 KB BSS for DES tables) — wizard mode disabled,
stub `xcrypt()` provided in the curses shim.

## Porting Checklist

1. Add upstream source as git submodule: `git submodule add <url> third_party/<app>`
2. Create `third_party/patches/<app>/` with any header overrides
3. Audit memory usage: `.data` + `.bss` must fit in 128 KB
4. Identify and stub unsupported features (e.g., networking, fork, mmap)
5. Write `third_party/build-<app>.sh` with cross-compilation flags
6. Add CMake integration: custom command + stamp file + romfs install
7. Test on QEMU first, then hardware

## Related Documentation

- [userland-dev-guide.md](userland-dev-guide.md) — User-space programming guide
- [syscall.md](syscall.md) — System call reference
- [architecture.md](architecture.md) — Kernel internals (ELF loader, PIE model)
- [history/port-rogue.md](history/port-rogue.md) — Rogue 5.4.4 porting details
