# PC/XT Core Layout: Split `.text` Out of DS=0

> Reclaim the ~36 KB the core `.text` currently steals from the DS=0
> data budget by linking and loading text into its own code segment
> only.  Targets `pcxt`; nothing changes for ARM/m68k/RISC-V/Xtensa.

---

## 1. Problem

The PC/XT core kernel links text+rodata+data+bss as one image starting
at DS=0 offset 0x0600.  Stage2 then loads that same image **twice** —
once to DS:0x0600 (so `SS=0`-relative data accesses resolve) and once
to a separate far segment at CS=0x1000 (so code executes out of its own
segment).  See [docs/targets/ia16.md §5](../targets/ia16.md) and the
header of [pcxt_kernel.ld](../../src/target/pcxt/kernel/core/pcxt_kernel.ld).

The DS=0 budget is hard-bounded by the VFS data reservation at 0xB600
and the kernel-stack region at 0xE380, leaving 45056 bytes for the
*entire* core image.  Today's image:

```
.text + .rodata     ~39256 B  ← redundant with the CS copy
.data                  368 B
.bss + COMMON         5490 B
                    ────────
Total                 45114 B   (against a 45056-byte budget)
```

The text-in-DS=0 share is ~87 % of the budget.  Adding any feature
(e.g. MS-DOS subsystem D-2abc) trips the `__bss_end <= 0xB600` link
assert because every byte of new code shrinks the BSS headroom.

The "BSS overflow" message in `ld` is misleading: BSS itself is fine,
it's the text *in front of it* that pushes the end address past
0xB600.

## 2. Why text is in DS=0 today

`pcxt_kernel.ld:42` carries this comment:

> `R_386_16` overflow … keeping code in a separate segment at runtime.

`ia16-elf-ld` emits 16-bit relocations (`R_386_16`) wherever 16-bit
near pointers are needed (function pointers in vtables, jump tables,
near-call targets).  If the linker linked `.text` at e.g. VMA
`0x10000`, those values would no longer fit in a 16-bit slot — hence
the workaround of linking everything into a single low region.

The trick the original layout used: link the whole image (text+data)
contiguously into DS=0, then have stage2 *additionally* copy the same
bytes into a far CS so code execution gets a separate segment.  The
DS=0 copy of `.text` is dead at runtime — it occupies budget and does
nothing.

## 3. Proposed Layout

The ideal split treats CS=0x1000 as the home of `.text`/`.rodata` and
DS=0 as the home of `.data`/`.bss` (plus existing reservations):

```
DS=0  (one 64 KB segment, holds shared data only)
  0x00000–0x004FF  IVT + BIOS data area
  0x00500–0x005FF  mod_info_t (written by stage2)
  0x00600–0x???    Core .data + .bss     (~5.8 KB today)
  0x0????          (free headroom — was ~0, now ~37 KB)
  0x0B600–0x0E37F  VFS .data + .bss      (existing reservation)
  0x0E380–0x0FFFF  Per-process kernel stacks (existing reservation)

CS=0x1000  (own 64 KB segment, holds core code only)
  0x10000–0x1????  Core .text + .rodata  (~39 KB)
  (no DS=0 duplicate — this is THE copy)

CS=????+   (page-aligned, holds VFS code only — unchanged)
```

`R_386_16` relocations within `.text` reference text symbols at offsets
`0x0000`..`text_end` (under 64 KB → fits 16 bits).  Relocations from
data into text (function pointers in tables) likewise reference
text-relative offsets and fit 16 bits.  Relocations into `.data`/`.bss`
reference DS=0 offsets `0x0600`..`0xB5FF` (under 64 KB → fits 16 bits).

The far-call infrastructure (CS = `seg_table[mod_id]`) already
provides the segment half of every cross-module call, so giving core
text its own VMA region is purely a link-time / loader change — no
runtime ABI change.

## 4. Concrete changes

### 4.1 `pcxt_kernel.ld`

Two output regions with separate VMAs:

```ld
SECTIONS {
  /* Code segment — VMA starts at 0, lives in CS=0x1000 at runtime. */
  . = 0x0000;
  .text : { *(.text.entry) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  __core_cs_end = .;

  /* Data segment — VMA at 0x0600, lives in DS=0 at runtime. */
  . = 0x0600;
  .data : { *(.data .data.*) }
  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss .bss.*) *(COMMON)
    KEEP(*subsys.c.obj(.bss))
    __bss_end = .;
  }
  /* …kernel stacks, page pool, VFS reservation as today… */

  ASSERT((__core_cs_end <= 0x10000),
         "ERROR: Core .text+.rodata exceeds 64 KB CS segment")
  ASSERT((__bss_end <= 0xB600),
         "ERROR: Core .data+.bss overflows into VFS reservation")
}
```

The two `ASSERT`s become independent — text-section growth no longer
threatens the BSS budget and vice versa.

### 4.2 `stage2.c`

Stop double-loading.  Today `stage2_main()` calls:

```c
load_file(kernel_ino, (uint8_t *)KERNEL_ADDR);   // DS=0:0x0600
load_file_far(kernel_ino, core_load_seg);        // CS=0x1000:0x0600
```

After:

```c
/* Code goes into CS only, starting at offset 0 of the segment. */
load_file_far(kernel_ino, core_cs /* 0x1000 */);

/* Data goes into DS=0 at 0x0600, read from a separate ELF section. */
load_file_data(kernel_ino, (uint8_t *)KERNEL_DATA_ADDR /* 0x0600 */);
```

The single ELF still contains both regions; stage2 needs to copy the
LOAD segments to their respective destinations.  Either:

- **(a) Two ELF program headers**, one with VMA=0 (text/rodata) and
  one with VMA=0x600 (data), and stage2 walks PHDRs to place each;
  *or*
- **(b) A flat binary with a known split offset** baked into the
  layout, so stage2 just streams one chunk to CS and one to DS.

Approach (a) is cleaner; we already use ELF program headers
elsewhere.  Approach (b) is simpler but couples stage2 to the link
layout.

### 4.3 `mod_info_t`

Already carries text and data base addresses.  Populate them honestly
so the kernel and seg manager know `core_cs_base` and `core_data_base`
are different.

### 4.4 `report_memory_usage.sh`

Split the single "core image in DS=0" line into two:

```
  core CS  (.text+.rodata):  ?????? / 65536 B   [CS 64 KB]
  core data in DS=0:         ??????  / ????? B   [0x0600..0xB600]
```

The core CS budget becomes the full 64 KB segment.  The core data
budget becomes ~45 KB (everything from 0x0600 up to the VFS
reservation), of which today's kernel uses ~5.8 KB.

### 4.5 Boot stub assumptions

`boot.S` and the early init code currently assume text and data live
in the same DS=0 region.  Audit:

- BSS clearing loop — uses `__bss_start`/`__bss_end` symbols.  These
  remain in DS=0 under the new layout, so the loop is unchanged.
- Initial `SP` setup — already comes from DS=0 (kernel stack region).
- Far-call patching — already operates on `seg_table` entries that
  the stage2 / `target_early_init` chain populates.

Most likely no source changes required outside the linker script,
stage2, and the `mod_info_t` plumbing.

## 5. Risks & unknowns

- **Where does `R_386_16` actually overflow?**  The original comment
  asserts overflow "would" occur; we don't know which symbol(s) caused
  it in the historical attempt.  The proposed layout dodges the
  obvious overflow case (data above 64 KB) by keeping each output
  region's VMA under 64 KB *within its own segment*.  If something
  still overflows, the most likely suspects are:
  - inline asm referencing a text symbol with an absolute 16-bit
    encoding;
  - jump tables emitted as 16-bit absolute words;
  - third-party / libgcc helpers that assume tiny model.
- **Stage2 placement.**  Stage2 itself currently lives at 0xC000 in
  DS=0.  The new layout doesn't displace stage2 (it's gone by the
  time the kernel runs), but the kernel's own initial `SP` lands in
  the kernel-stack region that begins at 0xE380 — make sure no
  intermediate state overlaps stage2 before control transfer.
- **Debug symbol fidelity.**  GDB and `addr2line` use the linked
  addresses; ensure the loaded VMAs match what the debugger expects
  (or document the offset).
- **Other ia16 targets.**  This proposal is `pcxt`-specific.  If the
  X68000 / Human68k path or CardComputer ever picks up an ia16 mode
  it will need its own layout decision.

## 6. Spike plan

Land in this order, each step independently verifiable:

1. **Spike the linker script** in a worktree branched from the latest
   commit.  Build kernel-only (no flashing).  Surface any
   `R_386_16` overflows; fix or document each.
2. **Update `stage2.c`** to load text and data separately; verify the
   kernel still boots in QEMU (banner + idle loop).
3. **Run the full pcxt test suite** (`./scripts/run.sh --test pcxt`) —
   16/16 must still pass.
4. **Update `report_memory_usage.sh` and `docs/targets/ia16.md`** to
   reflect the new layout.
5. **Re-attempt the MSDOS D-2abc rework** (currently stashed) on the
   freed-up budget; expect it to link without text-shaving.

## 7. Acceptance criteria

- `__core_cs_end` and `__bss_end` are independent constraints; growing
  one no longer affects the other.
- `report_memory_usage.sh` shows ≥30 KB of free headroom in *both* CS
  and DS=0 budgets after the existing kernel + MSDOS subsystem links.
- No regression on any existing pcxt test.
- The dead DS=0 copy of `.text` no longer exists (verified by a map
  file inspection: `__core_cs_end - 0x0000` should equal text+rodata
  size; `__bss_end - 0x0600` should equal data+bss size).

## 8. Out of scope

- Reorganizing the VFS data layout.
- Touching the user-space ABI or syscall interface.
- Any non-`pcxt` target.
- Reclaiming text-segment headroom for future feature growth (the 64
  KB CS budget is plenty; this proposal only frees DS=0).
