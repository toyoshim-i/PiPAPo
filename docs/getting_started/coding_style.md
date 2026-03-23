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

## Architecture- and Target-Specific Code

**Avoid `#ifdef` for arch/target conditionals.**  Do not scatter
`#ifdef __arm__` / `#ifdef __m68k__` or target-specific `#ifdef` guards
through shared kernel or driver code.  Instead, introduce an abstraction
(a common header declaring the interface) and provide per-arch or per-target
implementations in their own directories.

Architecture-specific implementations live under `src/arch/<arch>/`, and
target-specific implementations live under `src/target/<target>/`.  Shared
kernel code calls through dispatch headers (`arch/arch.h`, `arch/ioregs.h`)
or common APIs (`cpu/smp.h`) — never through preprocessor conditionals on
the architecture or target.

When adding a new arch- or target-specific feature:
1. Define a common interface (function prototype or struct) in a shared
   header under `src/kernel/`, `src/arch/`, or `src/drivers/`.
2. Add the implementation in `src/arch/<arch>/` or `src/target/<target>/`.
3. Wire each implementation into the appropriate source list in
   `cmake/kernel.cmake` (e.g., `ARCH_ARM_M_SOURCES`,
   `ARCH_M68K_SOURCES`, or the target's source list).
4. If you find existing `#ifdef` conditionals that can be replaced by this
   pattern, prefer refactoring them out.

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

## TODO Comments

Mark incomplete or temporary code with `TODO` comments so it can be found
and resolved later.  Format:

```c
// TODO: brief description of what needs to be done
```

```sh
# TODO: brief description of what needs to be done
```

Use `TODO` for:
- Workarounds that should be removed once an upstream issue is fixed.
- Missing features or error handling that will be added in a later step.
- Known limitations that are acceptable now but should be revisited.

Do **not** include author names or dates — `git blame` provides that
information.  Keep the description short and actionable.

## Compile-Time Flags for Work in Progress

When working on changes — even in the current worktree — always guard your
work-in-progress code behind a compile-time flag (e.g.,
`#if defined(ENABLE_MY_FEATURE)`).  This ensures that your uncommitted or
partially complete changes do not affect other agents or developers who may
be building and testing in parallel on the same workspace.

- Define a descriptive flag for your feature or change (e.g.,
  `ENABLE_XTENSA_PORT`, `ENABLE_NEW_SCHEDULER`).
- Keep the flag disabled by default; only enable it in your own build
  configuration (e.g., via `-D` in `cmake/user.cmake` or a target-specific
  CMake file).
- Remove the flag and make the code unconditional once the feature is
  complete, reviewed, and merged.

This practice prevents build breakage and test interference when multiple
agents work on the same tree concurrently.

## Formatting with clang-format

The repository includes a `.clang-format` file based on Google style.
Run it on all source files:

```sh
find src -name '*.c' -o -name '*.h' | grep -v third_party | xargs clang-format -i
```

Or via npx if clang-format is not installed system-wide:

```sh
find src -name '*.c' -o -name '*.h' | grep -v third_party | xargs npx clang-format -i
```
