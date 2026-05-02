# PiPAPo Coding Rules

## C Style

Based on the
[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
with these adjustments for embedded C:

- **Language**: C11 (GNU extensions allowed for inline asm).
- **Indentation**: 2 spaces, no tabs.
- **Braces**: same-line opening brace.
- **Naming**: `snake_case` for functions, variables, types.
  `UPPER_CASE` for macros/constants.  Typedef structs as `<name>_t`.
- **Line length**: 80 columns, strict.
- **Comments**: `/* */` for block, `//` for single-line.
- **Include order**: (1) corresponding header, (2) C stdlib,
  (3) project headers — alphabetical within each group.
- **Include paths**: always use full paths from `src/` root, never
  relative paths.  Example: `#include "kernel/vfs/procfs.h"`, not
  `#include "procfs.h"`.  This ensures the module boundary checker
  can detect cross-module violations by inspecting include strings.

This document overrides the Google guide on conflicts.

## Header Include Guards

Use `#ifndef` / `#define` / `#endif` (no `#pragma once`).

Derive the guard from the path relative to `src/`:
strip `src/`, replace `/` and `-` with `_`, uppercase, prefix `PPAP_`.

```c
/* src/kernel/vfs/klog.h */
#ifndef PPAP_KERNEL_VFS_KLOG_H
#define PPAP_KERNEL_VFS_KLOG_H
/* ... */
#endif /* PPAP_KERNEL_VFS_KLOG_H */
```

## Directory Layout

See [Source Tree Structure](source_tree.md).

## Architecture- and Target-Specific Code

**No `#ifdef` for arch/target conditionals in shared code.**
Define a common interface in a shared header, provide per-arch or
per-target implementations in `src/arch/<arch>/` or
`src/target/<target>/`, and wire them in cmake source lists.

## Code Quality

- **No code duplication.** Extract shared helpers when two or more
  arch/target paths share logic.  Three similar lines is better than
  a premature abstraction, but ten duplicated functions is not.
- **Fix root causes.** Avoid workarounds.  If a proper fix is too
  large, file a TODO with the root cause and intended fix.
- **No plan changes without discussion.** Stop and discuss blockers
  before switching approach or reverting agreed-upon work.
- **No commits without approval.** Always wait for explicit approval
  before `git commit` or `git commit --amend`.  Commits are review
  checkpoints.

## TODO Comments

```c
// TODO: brief actionable description
```

No author names or dates.  Use for workarounds, missing features, and
known limitations that will be revisited.

## Allocator Boundary

Code outside `src/kernel/core/mm/` must use `mem_region_*` only.
Direct `page_*` calls are forbidden outside mm.

Page-index conversions:
- `mem_region_page_linear(id)` -- `uint32_t`, safe on all targets.
- `mem_region_page_to_ptr(id)` -- `void *`, 32-bit only.
- `mem_region_page_read/write(id, off, buf, len)` -- safe on all
  targets including i16.

See [Memory Management](../kernel/memory.md) section 9.

Run `./scripts/check_allocator_boundaries.sh` before committing
allocator-related work.

## Compile-Time Flags for Work in Progress

Guard WIP code behind a compile-time flag
(`#if defined(ENABLE_MY_FEATURE)`) disabled by default.
Remove the flag once the feature is complete and merged.

## Formatting

Run clang-format (`.clang-format` in repo root):

```sh
find src -name '*.c' -o -name '*.h' | grep -v third_party \
  | xargs clang-format -i
```

## Commit Messages

### Format

```text
<scope>: <imperative summary>

<why + what changed + how verified>

Co-Authored-By: ...
```

### Rules

- Short, specific subject line.  Scope = subsystem/driver/target name
  (not `feat:`/`fix:`).
- Body: why, behavior impact, verification.  72-column wrap.
- Use a **real multi-line commit message**.  Do not put literal `\n`
  escape sequences into the body; pass separate `-m` paragraphs or use
  an editor / message file so the stored commit text contains actual
  newlines.
- One logical change per commit.

### Verification

Include in the commit body.

`./scripts/build.sh <target>` exits promptly -- use for mechanical
changes.  `./scripts/run.sh --test <target>` launches QEMU and does
**not** exit on its own -- always use with an external timeout.

```text
Verified with ./scripts/build.sh qemu_arm
Verified with ./scripts/run.sh --test qemu_arm (24/24 pass)
```

If not verified:

```text
Not verified by running tests (docs-only change)
```

### Co-Authored-By

Add trailers at the end of the commit message for contributors who
materially helped (code, design, debugging).

If an agent materially helped, add an explicit trailer for that agent
as well.  Agent contributors are encouraged to add their own names.
Examples:

```text
Co-Authored-By: GPT-5.4 Codex
Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
```

When an agent materially contributed, prefer adding the appropriate
`Co-Authored-By:` trailer by default rather than omitting it.

### Pre-commit checklist

1. Subject describes the behavioral change?
2. Body explains why?
3. Verification included?
4. Tests pass (or reason they were not run)?
5. Code follows this document?  Review [Code Quality](#code-quality):
   no duplication, no arch/target `#ifdef`, no direct `page_*` outside
   mm, no plan changes without discussion.
6. Affected docs updated?
7. `Co-Authored-By:` present when applicable?
8. Commit scoped tightly enough to review?
