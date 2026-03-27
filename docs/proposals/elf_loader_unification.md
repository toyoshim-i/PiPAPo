# ELF Loader Unification Proposal

## Summary

Eliminate compile-time `#ifdef` blocks from the ELF loader by pushing
arch-specific behavior behind the allocator, policy functions, and
relocation callbacks.

## Current State (after Steps 1-5)

The three per-arch functions (`elf_load_xtensa`, `elf_load_riscv`,
`elf_load_generic`) were merged into one `elf_load_image`.  However,
the file still has 44 `#ifdef` lines across these categories:

| Category | Lines | Description |
|----------|-------|-------------|
| `apply_relocations` | 17-117 | ARM/m68k-only REL/RELA handler at file scope |
| Xtensa static helpers | 127-418 | XIP layout analysis, section lookup |
| `elf_text_mode()` | 439-446 | Xtensa/RISC-V forced SRAM |
| `elf_copy_text/zero` | 452-474 | Xtensa word-copy vs memcpy |
| Segment classification | 492-518 | Xtensa literal segment |
| Text copy | 595-614 | Xtensa literal copy in IRAM region |
| `data_va` declaration | 632-634 | Only used by Xtensa/RISC-V relocs |
| Relocation blocks | 667-886 | Three separate `#ifdef` arch blocks |
| User stack tracking | 904-926 | RISC-V page track, m68k user_stack_page |
| m68k USP | 1043-1045 | `p->usp = argv_sp` |

## What Remains

### R-1: Unify relocations into callbacks

The largest `#ifdef` block (lines 667-886) contains three inline
relocation implementations.  Factor into per-arch functions with a
common signature and a runtime dispatch via `cpu_ops->arch_id`:

```c
typedef int (*elf_reloc_fn)(const elf32_ehdr_t *ehdr,
                            const uint8_t *file_buf, uint32_t file_size,
                            uint32_t text_base, uint8_t *data_base,
                            uint32_t text_end_va, uint32_t data_va,
                            uint32_t data_memsz,
                            const cpu_ops_t *cpu_ops, void *cpu_state,
                            elf_load_result_t *out);
```

This also absorbs `apply_relocations` (lines 17-117) into the ARM/m68k
callback, removing that file-scope `#if` block entirely.

Note: Xtensa and RISC-V relocations use the same split text/data
pattern (`addend < data_va → text_base, else → data_base`).  These
could share a common split-relocation helper.

### R-2: Unify copy helpers behind mem_region or mem_class

`elf_copy_text` / `elf_zero_text` use `#ifdef __xtensa__` to select
word-at-a-time vs memcpy.  This could be:
- A `mem_region_copy()` / `mem_region_zero()` API, or
- A dispatch on `mem_class` (RAM_TEXT on Xtensa needs word copy)

This removes the copy `#ifdef` blocks (lines 452-474).

### R-3: Unify segment classification

Xtensa literal segment detection (lines 492-518) is compile-time
guarded.  Since `literal_seg` is only meaningful on Xtensa and
harmless as NULL elsewhere, this can be a runtime check on
`cpu_ops->arch_id == CPU_ARCH_XTENSA`.

### R-4: Move Xtensa XIP helpers out of elf_loader

The Xtensa static helpers (lines 127-418) for XIP layout analysis
are large and Xtensa-specific.  They should move to a separate file
(e.g. `elf_xtensa.c` or `arch/xtensa/elf_reloc.c`) and be called
via the relocation callback.

### R-5: RISC-V XIP text

`elf_text_mode()` forces `ELF_TEXT_SRAM` for RISC-V because the gp
register setup (`load_base = data_base - data_va`) hasn't been
validated with XIP text.  Once validated, remove the `__riscv` guard
so ePIC binaries from romfs use `ELF_TEXT_XIP`.

### R-6: Xtensa PSRAM-first in allocator

The loader no longer handles PSRAM staging, but
`mem_region_alloc(RAM_TEXT)` on Xtensa still only allocates IRAM.
The allocator should try PSRAM first and fall back to IRAM.  This is
allocator work, not loader work.

### R-7: User stack / m68k USP conditionals

Small `#ifdef` blocks for RISC-V user stack page tracking (lines
904-921) and m68k `user_stack_page` / `usp` (lines 923-926,
1043-1045).  These are genuinely platform-specific but could
potentially be pushed into `proc_setup_stack` or arch hooks.

## Priority Order

1. **R-1** (relocation callbacks) — largest win, removes ~220 lines of `#ifdef`
2. **R-4** (move Xtensa helpers) — removes ~290 lines from elf_loader.c
3. **R-2** (copy helpers) — small cleanup
4. **R-3** (segment classification) — small cleanup
5. **R-5** (RISC-V XIP) — requires testing, can wait
6. **R-6** (PSRAM allocator) — separate subsystem
7. **R-7** (user stack / USP) — minor, low ROI

## Test Strategy

Same as before: `qemu_arm`, `qemu_rv32` after each change.
Xtensa and m68k when build environments are available.
