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
| R-2 | **Done** | Copy helpers use word-at-a-time unconditionally (safe on all arches, required for Xtensa IRAM) |
| R-3 | **Done** | Literal segment classification unified — two-pass scan for all arches, `literal_seg` is NULL on non-Xtensa |
| R-4 | **Done** | Dead Xtensa XIP helpers removed (~290 lines); XIP staging moved behind allocator |
| R-5 | **Done** | RISC-V uses ELF_TEXT_XIP from romfs; fixed brk=0 for XIP text with no data segment; gp concern moot (all current binaries are ET_EXEC, not ePIC ET_DYN) |
| R-6 | **Done** | `mem_region_alloc_ram_text` tries PSRAM (ext_text arena) first, falls back to IRAM; free path auto-detects which arena |
| R-7 | **Done** | RISC-V user stack tracking uses runtime check; m68k struct field guards are irreducible |

## Metrics

| Metric | Before | After |
|--------|--------|-------|
| `#ifdef` lines | ~100+ | **11** |
| File lines | 1382 | **~720** |
| Loader functions | 3 (per-arch) | 1 (`elf_load_image`) |

Remaining 11 `#ifdef` lines:
- 3: `elf_text_mode` Xtensa/RISC-V SRAM guard (R-5, blocked)
- 4: `elf_reloc_arch` per-arch definitions (irreducible — different reloc formats)
- 4: m68k `user_stack_page`/`usp` (irreducible — struct fields only exist on m68k)

## Open Items

### R-5: RISC-V XIP text (blocked)

`elf_text_mode()` forces `ELF_TEXT_SRAM` for RISC-V.  When XIP was
attempted, `test_exec` failed with data corruption.

Root cause: the ePIC crt0 computes `gp = load_base + __global_pointer$`.
With a contiguous image, `load_base` = start of the single SRAM block.
With split text (XIP in romfs) / data (SRAM), there is no single load
base.  Setting `load_base = data_base - data_va` should work in theory
but produced incorrect gp values in practice.

To unblock:
1. Inspect the ePIC crt0 startup code — how exactly does it use `sw[1]`
   (the gp seed in the switch frame) to compute `__global_pointer$`?
2. Verify with `objdump -d` that ePIC binaries don't embed absolute
   addresses in text (which would break XIP).
3. May need crt0 changes to accept split text/data bases.

### R-6: Xtensa PSRAM-first in allocator (open)

The loader removed XIP staging, expecting `mem_region_alloc(RAM_TEXT)`
on Xtensa to handle PSRAM vs IRAM selection internally.  The allocator
currently always returns IRAM.

To complete:
1. `mem_region_alloc_ram_text()` should try PSRAM first (via
   `heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_EXEC)`).
2. Fall back to the existing IRAM arena on failure.
3. This is allocator work (`mem_region.c`), not loader work.

## Test Strategy

`qemu_arm` and `qemu_rv32` after each change.
Xtensa (`xtensa_cc`) and m68k (`qemu_m68k`) when build environments
are available.
