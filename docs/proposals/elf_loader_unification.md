# ELF Loader Unification Proposal

## Summary

Replace the three per-arch ELF loader functions (`elf_load_xtensa`,
`elf_load_riscv`, `elf_load_generic`) with a single shared loader that
selects loading strategy per-segment based on runtime flags, not
compile-time `#ifdef`.

## Motivation

The current split into three functions is driven by loading strategy
differences, not true architectural differences:

1. **XIP text + SRAM data** — ARM/m68k today (text from romfs)
2. **Contiguous SRAM image** — RISC-V today (auipc needs relative offsets)
3. **Split text/data classes** — Xtensa today (IRAM vs DRAM)

These are not fixed per-arch.  ARM loading from SD card would need to
copy text to SRAM.  RISC-V with ePIC can now do XIP (text from romfs,
data in SRAM) — the same mode ARM/m68k uses today.  Xtensa with a
simpler binary could skip XIP staging.  The `mem_region` allocator
already abstracts memory class dispatch — the loader shouldn't
duplicate that knowledge.

## Key Insight

There are really only two loading modes per segment:

1. **XIP** — execute/read directly from the file buffer (romfs, flash)
2. **Copy to SRAM** — allocate via `mem_region` + copy

The choice is driven by **source capability** (romfs is XIP-capable,
floppy/HDD/SD card is not) and **binary format** (ePIC/PIC can XIP,
legacy non-PIC may need contiguous SRAM).  The `mem_class` for copied
segments is determined by segment flags; the allocator handles the rest.

Relocations are genuinely arch-specific but can be a callback.

## Target Architecture

### Loading Modes

Each segment gets a loading mode, determined at load time:

```c
typedef enum {
  ELF_TEXT_XIP,        /* Execute text in-place from file buffer */
  ELF_TEXT_SRAM,       /* Copy text to SRAM (PPAP_MEM_RAM_TEXT) */
} elf_text_mode_t;
```

Data segments always go to SRAM via `mem_region_alloc(PPAP_MEM_RAM_DATA)`
— there is no data XIP case.

A policy function decides the text mode:

```c
static elf_text_mode_t elf_text_mode(const elf32_ehdr_t *ehdr,
                                     int source_is_xip_capable);
```

Today's mapping:
- XIP-capable source + PIC/ePIC binary → `ELF_TEXT_XIP`
- XIP-capable source + Xtensa → `ELF_TEXT_SRAM` (IRAM via allocator)
- Non-XIP source (floppy, HDD, SD card) → `ELF_TEXT_SRAM`

### Memory Class Selection

A second policy function maps segment flags to `ppap_mem_class_t`:

```c
static ppap_mem_class_t elf_segment_mem_class(const elf32_phdr_t *seg);
```

- `PF_X` → `PPAP_MEM_RAM_TEXT`
- `PF_W` → `PPAP_MEM_RAM_DATA`
- read-only → `PPAP_MEM_RAM_RODATA`

The allocator dispatches these to IRAM/DRAM on Xtensa, page-backed
SRAM elsewhere.  The loader doesn't know or care.

### No Contiguity Constraint

The legacy RISC-V non-ePIC mode (contiguous SRAM for entire image)
is removed.  All RISC-V user-space binaries use ePIC, which supports
XIP and per-segment allocation like ARM/m68k.

### Copy Semantics

The default copy is `memcpy`.  Xtensa IRAM requires word-at-a-time copy.
This can be a per-mem-class copy function provided by the memory
subsystem or a small inline helper selected by `mem_class`.

### Relocation Callbacks

Relocations are genuinely arch-specific.  Factor them into per-arch
static functions:

```c
typedef int (*elf_reloc_fn)(const elf32_ehdr_t *ehdr,
                            const uint8_t *file_buf, uint32_t file_size,
                            uint8_t *text_base, uint8_t *data_base,
                            uint32_t text_vaddr, uint32_t data_vaddr,
                            const cpu_ops_t *cpu_ops, void *cpu_state);
```

Each arch registers its relocation function.  The shared loader calls
it after all segments are loaded.

### No Xtensa XIP Staging in the Loader

Xtensa PSRAM text staging is moved behind the allocator.
`mem_region_alloc(PPAP_MEM_RAM_TEXT)` on Xtensa tries PSRAM first and
falls back to IRAM when PSRAM is not available.  The loader just
requests executable memory and copies text there — it does not know
or care whether the allocation landed in PSRAM or IRAM.

This eliminates `staged_text`, `staged_rodata`, the PSRAM exec
feature flags, and the fallback logic from the loader entirely.

## Unified Loader Sketch

```
elf_load_unified(p, ehdr, file_buf, ...):
  1. Classify segments (text, data, literal)
  2. Determine loading mode per segment (XIP vs copy)
  3. If CONTIGUOUS: single mem_region_alloc for full extent
     Else: per-segment mem_region_alloc (by mem_class)
  4. Copy non-XIP segments (memcpy or word-copy based on mem_class)
  5. Allocate stacks (kernel + optional user via mem_region)
  6. Call arch relocation function
  7. Set up brk, image segments, entry point
  (No Xtensa XIP staging step — handled by allocator)
```

## Migration Plan

### Step 1: Introduce policy functions

Add `elf_segment_mode()` and `elf_segment_mem_class()`.  Wire them
into the existing per-arch functions without changing behavior.
Verify all targets pass.

### Step 2: Unify allocation + copy

Replace the three allocation+copy blocks with a shared loop that
uses the policy functions.  Handle the contiguity flag for RISC-V.
The Xtensa word-copy can be a conditional within the copy step.

### Step 3: Extract relocation callbacks

Factor Xtensa, RISC-V, and ARM/m68k relocation code into separate
`elf_reloc_*` functions with a common signature.  The shared loader
calls the appropriate one.

### Step 4: Merge into single function

Remove the three per-arch functions.  The shared loader handles all
architectures through the policy functions and relocation callback.
Xtensa XIP staging remains a post-load hook.

### Step 5: Make XIP a runtime decision

Add `source_is_xip_capable` parameter based on the file source
(romfs → yes, tmpfs/SD → no).  ARM loading from non-XIP sources
falls back to `ELF_SEG_COPY` automatically.

## Risks

- Legacy RISC-V non-ePIC binaries will no longer load (by design).
- Xtensa IRAM word-copy: must not regress to byte copy silently.
- GOT patching on ARM/m68k uses `cpu_ops->read32/write32` — this is
  an emulated-CPU concern.  The relocation callback can handle it.

## Test Strategy

Same as allocator migration: smoke test on `qemu_arm`, `qemu_m68k`,
`qemu_rv32`, `xtensa_cc` after each step.

## Relationship to Allocator Migration

This proposal is independent of but complementary to the allocator
migration (Phase 1 is already complete).  It builds on the fact that
all ELF loader paths already use `mem_region_*`, making the
unification possible.
