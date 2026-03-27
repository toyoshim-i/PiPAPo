# Allocator Migration Proposal

## Summary

This proposal unifies kernel memory allocation under a single public API:
`mem_region_*`.

The current tree uses two public allocator families:

- `page_*` calls are spread across many subsystems.
- `mem_region_*` is used by newer paths (notably Xtensa ELF loading) and
  process image ownership cleanup.

Goal: make `mem_region_*` the only allocator API used outside the memory
management subsystem.

This does **not** remove the page allocator implementation. `page_*` remains
an internal backend used by `mem_region_*` where appropriate.

## Status

| Phase | Status | Notes |
|-------|--------|-------|
| Phase 0 | Done | Proposal + `page.h` guardrail landed |
| Phase 1 | Done | `kernel/exec/elf_loader.c` migrated to `mem_region_*` |
| Phase 2 | Done | `kernel/exec/exec.c` and remaining loaders migrated |
| Phase 3 | Done | Process/syscall paths migrated to `mem_region_*` |
| Phase 4 | Done | Residual subsystem and bootstrap users migrated |
| Phase 5 | Open | Enforcement not added yet |

Most recent landing steps:

- `c23f18a` — migrate Human68k `x_loader` / `r_loader`
- `82bbee1` — migrate remaining exec loaders and `exec.c`
- `51e0433` — migrate process and syscall allocator users

## Why This Change

- Enforces one allocator contract for all subsystems.
- Centralizes arch-specific behavior in one place (`mem_region.c`).
- Reduces allocator misuse and mixed ownership bugs.
- Improves long-term portability and reviewability.

## Current State (Observed)

- `mem_region_init()` is called during boot in `kernel/main.c`.
- `mem_region_alloc()/free()` already support:
  - Generic page-backed allocations on non-Xtensa targets.
  - Xtensa-specific internal/external arenas.
- `sys_proc` cleanup already releases owned process-image segments with
  `mem_region_free()`.
- Direct `page_*` allocation calls are now confined to `src/kernel/mm/`.
- Subsystems outside the memory backend now allocate and free through
  `mem_region_*`, including contiguous page-backed users.

## Non-Goals

- No big-bang rewrite in one change.
- No behavior change in target memory semantics.
- No removal of `page.c` backend logic.

## Target Architecture

### Public API Boundary

- Allowed outside `src/kernel/mm/`:
  - `mem_region_alloc`
  - `mem_region_alloc_at`
  - `mem_region_free`
- Not allowed outside `src/kernel/mm/`:
  - `page_alloc`
  - `page_alloc_contiguous`
  - `page_alloc_at`
  - `page_free`

### Backend Layer

`mem_region.c` dispatches by `ppap_mem_class_t`:

- Generic classes: page-backed implementation.
- Xtensa classes: dedicated arena allocators.

This keeps minimal architecture conditionals in one backend file.

## Migration Plan

## Phase 0: Guardrails and Inventory

- Add a clear header comment in `page.h` that `page_*` is backend-only and
  new code outside `src/kernel/mm/` must use `mem_region_*`.
- Keep a tracked inventory of all direct `page_*` users.

Deliverables:

- This proposal document.
- Header guidance in the memory module.

## Phase 1: First Real Migration (`elf_loader`)

Status: **Done**

- Rewrite `kernel/exec/elf_loader.c` to use `mem_region_*` consistently.
- Keep the current target memory semantics unchanged.
- Prefer minimal `#ifdef` only where an architecture-specific ELF detail
  cannot be expressed cleanly through shared control flow.

Deliverables:

- No direct `page_*` calls in `kernel/exec/elf_loader.c`.
- `elf_loader` remains shared code, with only minimal unavoidable
  architecture conditionals.
- Smoke-tested on all primary targets.

## Phase 2: Remaining Exec Path Migration

Status: **Done**

Migrate these first:

- `kernel/exec/exec.c`
- `kernel/exec/{x,r,com,sos,flat,m68k_emu}_loader.c`

Rules:

- Replace direct `page_*` alloc/free with `mem_region_*`.
- Assign explicit `ppap_mem_class_t` per segment.
- Use `PROC_IMAGE_SEG_OWNED` where ownership is transferred.

Deliverables:

- No direct `page_*` calls in `kernel/exec/`.
- `x_loader`, `r_loader`, `com_loader`, `sos_loader`, `flat_loader`,
  `m68k_emu_loader`, and `exec.c` now use `mem_region_*`.
- Build-smoke verified on:
  - `qemu_arm`
  - `qemu_m68k`
  - `ibmpc`

## Phase 3: Process + Syscall Migration

Status: **Done**

Migrate:

- `kernel/syscall/sys_proc.c`
- `kernel/syscall/sys_mem.c`
- `kernel/syscall/syscall.c`

Rules:

- Allocate/free stacks and process-owned pages via `mem_region`.
- Keep existing `brk` and page tracking semantics unchanged.

Deliverables:

- No direct `page_*` calls in process/syscall paths.

## Phase 4: Remaining Subsystems

Status: **Done**

Migrate residual users:

- `kernel/fs/tmpfs.c`
- `kernel/cpu/ecpu_*.c`
- `arch/*/smp.c`
- target bootstrap helpers using `page_alloc_at`

Deliverables:

- No direct `page_*` calls outside `src/kernel/mm/`.
- `tmpfs`, `procfs`, `human68k_bridge`, emulator state allocators,
  ARM SMP startup, and target bootstrap reservations now use `mem_region_*`
  or `mem_region` query helpers.

## Phase 5: Enforcement

Status: **Open**

- Optionally hide `page.h` from non-mm include paths.
- Update coding rules to mandate allocator API usage.

Deliverables:

- Enforced single allocator API policy.

## Compatibility and Risk Notes

- RISC-V currently relies on contiguous image allocations in exec path.
  Migration must preserve contiguity guarantees.
- Xtensa text/data/staged segment handling must preserve current mem_class
  mapping and staged execution behavior.
- `mem_region_free_tracked_page()` behavior should remain valid for any page
  tracked through process page tables.

## Test Strategy

For each phase, run at minimum:

- Build and boot smoke:
  - `qemu_arm`
  - `qemu_m68k`
  - `qemu_rv32`
  - `xtensa_cc`
- Exec smoke:
  - launch init/shell
  - run sample binaries
- Lifecycle smoke:
  - fork/vfork, exec, exit, waitpid
- Memory smoke:
  - repeated exec loops
  - brk growth/shrink
  - allocator failure paths

## Acceptance Criteria

- Only `mem_region_*` allocator API is used outside `src/kernel/mm/`.
- `page_*` remains backend-only implementation detail.
- No functional regressions in boot, exec, process lifecycle, and memory syscalls.
- CI guardrail prevents reintroduction of direct `page_*` usage.

## Rollout and Backout

Rollout:

- Land phase-by-phase with small reviewable patches.
- Keep each phase bisectable and independently testable.

Backout:

- Revert the latest migration phase only.
- Keep `mem_region` wrappers backward-compatible during migration window.

## Open Questions

- Should we enforce `mem_region` usage in `src/arch/*` immediately, or allow
  a temporary exception list through Phase 4?
- Should `mem_region_alloc_at` gain richer diagnostics for fragmented failures?
- Do we need additional `mem_class` values for emulator state pages, or reuse
  `PPAP_MEM_RAM_DATA` consistently?
