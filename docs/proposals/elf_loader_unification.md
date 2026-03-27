# ELF Loader Unification Proposal

## Summary

Eliminate compile-time `#ifdef` blocks from the ELF loader by pushing
arch-specific behavior behind the allocator, policy functions, and
relocation callbacks.

## Status

| Item | Status | Description |
|------|--------|-------------|
| Steps 1-5 | **Done** | Three per-arch loaders merged into `elf_load_image`; `elf_text_mode()` policy; runtime XIP via `EXEC_FLAG_XIP_SOURCE` |
| R-1 | **Done** | Relocation callbacks extracted (`elf_reloc_arch` + `elf_reloc_ctx_t`); `apply_relocations` absorbed; shared `elf_split_addr` + `elf_reloc_got_split` helpers |
| R-2 | Open | Copy helpers (`elf_copy_text`/`elf_zero_text`) still use `#ifdef __xtensa__`; needs allocator-side `mem_region_copy`/`mem_region_zero` |
| R-3 | **Done** | Literal segment classification unified — two-pass scan for all arches, `literal_seg` is NULL on non-Xtensa |
| R-4 | Open | Xtensa XIP helpers (~290 lines) still in elf_loader.c |
| R-5 | Open | RISC-V forced to `ELF_TEXT_SRAM`; XIP needs gp validation |
| R-6 | Open | Xtensa allocator doesn't try PSRAM first yet |
| R-7 | Open | User stack / m68k USP `#ifdef` blocks (~20 lines) |

Current `#ifdef` count: **28** (down from 39 after R-1, ~100+ before Steps 1-5).

## What Remains

### R-2: Unify copy helpers (small)

`elf_copy_text` / `elf_zero_text` use `#ifdef __xtensa__` to select
word-at-a-time vs memcpy.  Options:
- A `mem_region_copy()` / `mem_region_zero()` API, or
- A dispatch on `mem_class` (RAM_TEXT on Xtensa needs word copy)

### R-3: Unify segment classification (small)

Xtensa literal segment detection is compile-time guarded.  Since
`literal_seg` is only meaningful on Xtensa and harmless as NULL
elsewhere, this can be a runtime check on
`cpu_ops->arch_id == CPU_ARCH_XTENSA`.

### R-4: Move Xtensa XIP helpers out of elf_loader

The Xtensa static helpers (~290 lines) for XIP layout analysis
should move to a separate file (e.g. `arch/xtensa/elf_xtensa.c`)
and be called via the relocation callback.

### R-5: RISC-V XIP text

`elf_text_mode()` forces `ELF_TEXT_SRAM` for RISC-V because the gp
register setup (`load_base = data_base - data_va`) hasn't been
validated with XIP text.  Once validated, remove the `__riscv` guard.

### R-6: Xtensa PSRAM-first in allocator

`mem_region_alloc(RAM_TEXT)` on Xtensa should try PSRAM first and
fall back to IRAM.  Allocator work, not loader work.

### R-7: User stack / m68k USP conditionals (minor)

Small `#ifdef` blocks for RISC-V user stack page tracking and m68k
`user_stack_page` / `usp`.  Genuinely platform-specific; low ROI
to eliminate.

## Test Strategy

Same as before: `qemu_arm`, `qemu_rv32` after each change.
Xtensa and m68k when build environments are available.
