# PiPAPo Coding Rules

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

## Code Quality

- **Keep code clean**: refactor as you go.  When touching an area, leave
  it better than you found it.
- **Required refactoring**: if adding a feature creates duplication or
  makes an existing pattern harder to follow, refactor first (or in the
  same commit).  Do not defer cleanup that the next reader will trip over.
- **Avoid code duplication**: when two or more architectures share the
  same logic (e.g., user-stack copy in vfork, split-address relocation),
  extract a shared helper and call it from the arch-specific paths.
- **Diverge per-arch by introducing the right abstraction**: instead of
  duplicating a function with small per-arch tweaks, factor out the
  common algorithm into a shared function and pass arch-specific details
  via parameters, callbacks, or per-arch constants.
- **Fix root causes, not symptoms**: when a bug or limitation surfaces,
  invest in the essential fix that addresses the underlying design issue.
  Avoid short-term ad-hoc workarounds that paper over the problem —
  they accumulate technical debt and make the real fix harder later.
  If a proper fix is too large for the current step, file a TODO with
  a clear description of the root cause and the intended fix.

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

## Allocator Boundary

Code outside `src/kernel/mm/` must allocate through `mem_region_*`.

- Allowed outside `src/kernel/mm/`:
  - `mem_region_alloc`
  - `mem_region_alloc_at`
  - `mem_region_free`
  - `mem_region_*` query helpers
- Not allowed outside `src/kernel/mm/`:
  - `page_alloc`
  - `page_alloc_at`
  - `page_alloc_contiguous`
  - `page_free`
  - `page_free_count`
  - `page_max_contiguous`

`page_*` remains a backend implementation detail.  If non-mm code needs
capacity or free-space information, add an appropriate `mem_region_*`
helper instead of reaching into `page.h` directly for allocator state.

Run the boundary checker before committing allocator-related work:

```sh
./scripts/check_allocator_boundaries.sh
```

### Page-index conversions

Per-process memory is tracked by `page_id_t` (uint16_t index), not by
raw pointers.  Two functions convert an index back to an address — use
the right one for portability:

- **`mm_page_linear(id)`** → `uint32_t`: safe on all targets including
  i16.  Use for arithmetic, comparisons, and reporting.
- **`mm_page_to_ptr(id)`** → `void *`: 32-bit targets only (unavailable
  on i16).  Use only when you need a dereferenceable pointer.

See [Memory Management Refactoring](../proposals/memory_management.md)
§6 for the full conversion rules and rationale.

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

## Commit Messages

This project uses short, scoped subjects plus clear bodies that explain
behavior changes and how they were validated.

### Structure

```text
<scope>: <one-line summary in imperative mood>

<why this change is needed>
<what changed, focusing on behavior and risks>
<how you verified the change>
<extra context only if needed>

Co-Authored-By: <Agent name> (<model name>) [<optional valid email>]
```

### Rules

- Keep the first line short and specific.
- Leave one blank line after the subject.
- Wrap body lines to about 72 columns.
- Prefer "why + behavior impact" over implementation trivia.
- Keep one logical change per commit when possible.

### Subject line convention

Use a scope prefix that clearly identifies the area of the change. Prefer
specific, descriptive scopes over generic category words like `feat:` or
`fix:`. The scope should tell the reader *what part of the system* changed
at a glance.

Examples from recent history:

- `semihost: add ARM semihosting serial backend`
- `pico2: enable SMP Core 1 launch on RP2350`
- `signal: correct signal delivery for FPU-active processes`
- `exec: support PIE relocation for m68k ELF binaries`
- `romfs: fix directory traversal past end of image`
- `build: add PPAP_ENABLE_CPM build flag`
- `docs: update arm_m.md with RP2350 MPU details`
- `test: add pipe stress test for large writes`

General guidelines:

- Pick the scope from the feature, subsystem, driver, or target name —
  not from the type of change (avoid `feat:`, `fix:`, `refactor:` as
  the sole scope).
- If multiple areas are touched equally, choose the dominant behavior
  change or use the most specific applicable scope.
- An unscoped subject is acceptable when no single scope fits.

### Body content

Include the details reviewers and future maintainers need:

- Previous behavior (or bug).
- New behavior after this commit.
- Any compatibility or regression risk.
- Follow-up work if this is part of a larger series.

Avoid:

- Repeating obvious diffs ("renamed X to Y") without why.
- Large narrative text not tied to behavior changes.

### Verification

Include verification details in the commit body instead of a required trailer.
Keep it concise and concrete.

Examples:

```text
Verified with ./scripts/run.sh --test qemu_arm
Verified with ./scripts/run.sh --test qemu_m68k
Verified by building qemu_m68k target and checking boot output
```

If tests were not run, be explicit:

```text
Not verified by running tests (docs-only change)
```

### Co-Authored-By protocol

Add `Co-Authored-By:` trailers when another contributor materially helped
create the commit (code, design, debugging, or substantial text).

- Put trailers at the very end of the commit message.
- Keep a blank line between the body and trailers.
- One trailer line per contributor.

For human contributors:

```text
Co-Authored-By: Jane Doe <jane@example.com>
```

For AI agent-assisted commits:

```text
Co-Authored-By: <Agent name> (<model name>) [<optional valid email>]
```

### Pre-commit checklist

1. Does the subject describe the behavioral change clearly?
2. Does the body explain why this change exists?
3. Does the body include how the change was verified (or why not)?
4. Did affected tests pass (or is there a clear reason they were not run)?
5. Does the code follow this coding rules document?
   In particular, check that no new `#ifdef` conditionals on arch or target
   have been introduced — prefer per-arch/per-target implementations instead.
   Also check that non-mm code does not introduce new direct `page_*` calls.
6. Are affected documents updated?
7. Are `Co-Authored-By:` trailers present when applicable?
8. Is this commit scoped tightly enough to review easily?
