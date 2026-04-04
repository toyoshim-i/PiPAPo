# Source Tree Structure

This document describes the PPAP source tree layout and the rules
governing include paths and module boundaries.

---

## Top-Level Layout

```
src/
  common/            User-kernel shared types (syscall_nr.h, dirent.h, ...)
  etc/               Default /etc files (profile, fstab, inittab)
  kernel/            Kernel code, split into core/vfs/common
  arch/<arch>/       Architecture-specific overlays
  target/<target>/   Target-specific overlays
  user/              Architecture-independent user-space programs
```

---

## Kernel Modules

The kernel is split into three directories that reflect the module
boundary enforced on ia16 (where core and VFS are separate binaries):

```
kernel/
  common/            Shared between core and VFS
    mod/             Module interfaces (mod_core.h, mod_vfs.h, mod_vfs.inc)
    arch.h           Architecture dispatcher (includes kernel/core/arch.h via overlay)
    ioregs.h         I/O register dispatcher
    config.h         Build configuration
    errno.h          Error codes
    seg.c/h          Segment manager
    spinlock.h       SMP spinlock / core_id()
  core/              Core module
    main.c           Kernel entry point
    klog.c/h         Kernel logger
    endian.h         Byte-order helpers
    mm/              Memory management (page, kmem, mem_region, mpu, xip)
    proc/            Process management and scheduler
    signal/          Signal delivery
    exec/            Binary loaders (ELF, flat, ELF16, COM, X/R/SOS/H68K)
    syscall/         System call dispatch and handlers
    subsys/          Subsystem bridges (Human68k, CP/M, SOS)
    cpu/             CPU abstraction and emulated CPUs (m68k, z80)
    driver/          Shared drivers (blkdev, uart, fbcon, i2c, spi, lcd, ...)
  vfs/               VFS module
    vfs.c/h          VFS layer and mount table
    namei.c          Path resolution
    vfs_types.h      VFS type definitions
    fd.c/h           File descriptor pool
    file.h           struct file and file_ops
    tty.c/h          TTY driver
    pipe.c           Pipe implementation
    devfs.c/h        /dev filesystem
    procfs.c/h       /proc filesystem
    tmpfs.c/h        In-memory filesystem
    romfs.c/h        Read-only filesystem (flash/XIP)
    ufs.c/h          Unix filesystem (block device)
    vfat.c/h         FAT filesystem
    fstab.c/h        /etc/fstab automount
```

### Module Boundary Rules

- **Core code** may include `core/` and `common/` headers.
- **VFS code** may include `vfs/` and `common/` headers.
- Cross-module **function calls** go through `mod_core` / `mod_vfs`
  interfaces (far calls on ia16, direct calls on 32-bit).
- Cross-module **type definitions** are shared via `common/` headers
  (data types only, no function prototypes).

---

## Architecture Overlays

Each architecture directory mirrors the primary `src/` tree:

```
arch/<arch>/
  boot/              Boot code (boot.S, stage1.S)
  kernel/core/       Arch-specific kernel core (arch.h, switch.S, trap.S, ...)
    driver/          Arch-specific drivers (uart_rpico.c, clock_rpico.c, ...)
  user/              Arch-specific user code (crt0.S, syscall.S, user.ld)
```

### Supported Architectures

| Directory    | CPU          | Targets                     |
|-------------|--------------|------------------------------|
| `arm_m/`    | ARM Cortex-M | qemu_arm, pico1, pico1calc, pico2 |
| `m68k/`     | Motorola 68k | qemu_m68k, x68k             |
| `riscv/`    | RISC-V RV32  | qemu_rv32, pico2rv          |
| `xtensa/`   | Xtensa LX7   | xtensa_cc (ESP32-S3)        |
| `i16/`      | 8086/V30     | pcxt                        |

---

## Target Overlays

Each target directory mirrors the primary `src/` tree:

```
target/<target>/
  boot/              Boot loaders (stage1, stage2, linker scripts)
  kernel/
    core/            Target-specific kernel (target_*.c, config headers, linker scripts)
      driver/        Target-specific drivers (uart, timer, floppy, etc.)
    common/          Target-specific shared code (ia16 module stubs on pcxt)
      stubs/         Far-call stubs (pcxt only)
    vfs/             Target-specific VFS (linker scripts on pcxt)
  user/              Target-specific user programs
  romfs/             Root filesystem overlay (/etc/inittab, /etc/profile)
  esp_idf/           ESP-IDF project files (xtensa_cc only)
```

Shared target interface files live at the top:
- `target/target.h` -- target hook declarations
- `target/target_default.c` -- default implementations

---

## Include Path Convention

The build system specifies three include paths for each target:

```
-I src/
-I src/kernel/
-I src/arch/<arch>/
-I src/target/<target>/
```

This creates a resolution chain where arch and target directories
**overlay** the primary tree.  For example:

```c
#include "kernel/core/arch.h"
```

The compiler searches in order:

1. `src/kernel/core/arch.h` -- not found (arch.h lives in overlays)
2. `src/arch/<arch>/kernel/core/arch.h` -- found (arch-specific)

Similarly for target-specific drivers:

```c
#include "kernel/core/driver/bios_con.h"
```

1. `src/kernel/core/driver/bios_con.h` -- not found (target-specific)
2. `src/target/pcxt/kernel/core/driver/bios_con.h` -- found

### Include Rules

- **No relative includes** (`../`).  All `#include` paths are resolved
  through the include path, not relative to the including file.
- Use `"common/..."` for kernel-common headers (via `-I src/kernel/`).
- Use `"core/..."` for core module headers (via `-I src/kernel/`).
- Use `"vfs/..."` for VFS module headers (via `-I src/kernel/`).
- Use `"kernel/core/..."` for arch/target overlay resolution (via `-I src/arch/<arch>/`).
- Use `"common/syscall_nr.h"` etc. for user-kernel shared types (via `-I src/`).

---

## User Space

Architecture-independent user programs live in `src/user/`:

```
user/
  push.c             Shell (PiPAPo micro-shell)
  push_line.c        Line editor with VT100 support
  init.c             Init process
  hello.c            Hello world
  ...
```

Architecture-specific user code (syscall stubs, crt0, linker scripts)
lives in `arch/<arch>/user/`.
