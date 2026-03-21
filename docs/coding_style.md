# PiPAPo Coding Style

## Header Include Guards

Every header file uses traditional `#ifndef` / `#define` / `#endif` guards
(no `#pragma once`).

The guard macro is derived mechanically from the file path relative to `src/`:

```
src/<path>/<name>.h  ->  PPAP_<PATH>_<NAME>_H
```

- Strip the leading `src/`.
- Replace `/` and `-` with `_`.
- Convert to upper case.
- Prefix with `PPAP_`.

Examples:

| File                              | Guard                                  |
|-----------------------------------|----------------------------------------|
| `src/kernel/fs/tmpfs.h`          | `PPAP_KERNEL_FS_TMPFS_H`              |
| `src/arch/arm_m/ioregs.h`       | `PPAP_ARCH_ARM_M_IOREGS_H`           |
| `src/drivers/spi_sd.h`          | `PPAP_DRIVERS_SPI_SD_H`              |
| `src/target/pico1/pico1.h`      | `PPAP_TARGET_PICO1_PICO1_H`          |
| `src/user/syscall.h`            | `PPAP_USER_SYSCALL_H`                |

The closing `#endif` must include the guard name as a comment:

```c
#ifndef PPAP_KERNEL_FS_TMPFS_H
#define PPAP_KERNEL_FS_TMPFS_H

/* ... */

#endif /* PPAP_KERNEL_FS_TMPFS_H */
```

## Directory Layout

```
src/
  arch/           Architecture abstraction layer
    arch.h          Dispatch header -> arm_m/arch.h or m68k/arch.h
    ioregs.h        Dispatch header -> arm_m/ioregs.h or m68k/ioregs.h
    arm_m/          ARM Cortex-M (boot, switch, trap, SMP, ioregs)
    m68k/           Motorola 68000 (boot, switch, trap, SMP, ioregs)
  common/         Headers shared between kernel and userland
  config.h        Build configuration
  drivers/        Device drivers (uart, spi, i2c, lcd, sd, ...)
    arch/arm_m/     ARM-specific driver implementations
  kernel/         Kernel core
    blkdev/         Block device layer
    cpu/            eCPU abstraction (emulated CPU vtable, Z80/m68k emulators)
    exec/           ELF/flat/X/R-format loaders
    fd/             File descriptors, TTY, pipes
    fs/             Filesystems (romfs, tmpfs, devfs, procfs, vfat, ufs)
    mm/             Memory management (pages, kmem, MPU, XIP)
    proc/           Process table, scheduler
    signal/         Signal delivery
    subsys/         Personality subsystems (Human68k, CP/M, S-OS)
    syscall/        Syscall dispatch
    vfs/            Virtual filesystem layer
  target/         Per-board target configuration
  user/           Userland programs and libraries
```

Key distinction:
- `arch/ioregs.h` -- memory-mapped I/O register definitions (SysTick, SCB, NVIC, SR, ...)
- `kernel/cpu/cpu.h` -- emulated CPU (eCPU) abstraction layer and vtable

## Architecture-Specific Code

Architecture-specific implementations live under `src/arch/<arch>/`, not in
kernel directories behind `#ifdef`.  Shared kernel code calls through the
dispatch headers (`arch/arch.h`, `arch/ioregs.h`) or the common API
(`cpu/smp.h`).

When adding a new arch-specific feature:
1. Add the implementation in `src/arch/<arch>/`.
2. If it needs a shared API, add a header under `src/kernel/` and
   arch-specific `.c` files under each `src/arch/<arch>/`.
3. Wire both implementations into `ARCH_ARM_M_SOURCES` / `ARCH_M68K_SOURCES`
   in `cmake/kernel.cmake`.

## C Style

This project follows the
[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
with the following project-specific adjustments for embedded C:

- **Language**: C11 (GNU extensions allowed for inline asm).
- **Indentation**: 2-space indentation, no tabs in `.c`/`.h` files.
- **Braces**: Opening brace on the same line for functions and control flow.
- **Naming**:
  - `snake_case` for functions, variables, and types (not `CamelCase` as in
    Google C++ style, since this is a C project).
  - `UPPER_CASE` for macros and constants.
  - Typedef structs as `<name>_t` (e.g., `pcb_t`, `vfs_ops_t`).
- **Line length**: 80 columns, strict.
- **Comments**: Use `/* */` for block comments and `//` for single-line
  comments. Follow Google style for comment placement and formatting.
- **Header include order** (following Google style):
  1. Corresponding header (e.g., `foo.c` includes `foo.h` first).
  2. C standard library headers (`<stdint.h>`, `<string.h>`, ...).
  3. Project headers (`"arch/arch.h"`, `"kernel/klog.h"`, ...).

Where the Google C++ Style Guide and this document conflict, this document
takes precedence.
