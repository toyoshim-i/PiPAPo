# Proposal: Xtensa XIP for User Binaries

> **Status**: Not started.  Build pipeline already produces XIP-layout
> ELF variants; runtime loader does not consume them.  This proposal
> covers the loader, romfs, and tuning changes needed to flip xtensa
> from RAM-loaded to flash-XIP user binaries.

## Summary

Today the xtensa loader copies every user binary's `.text`,
`.rodata`, `.data`, `.bss`, and stack into runtime memory before
exec.  The text + rodata copy lands in the IRAM `ram_text` arena
(see [docs/targets/xtensa.md §4](../targets/xtensa.md#cooperative-ram-use-with-esp-idf)),
which forces a 128 KB IRAM rental that, in turn, pressures the SRAM1
dual-mapping safeguards down to a 24-page (96 KB) DRAM page pool.
That pool is too small to host a contiguous 64 KB Z80 emulator
instance, which is the underlying reason `test_cpm` and `test_sos`
fail at `execve` on xtensa.

Map text and rodata directly out of flash via the I-bus instead.
ESP32-S3 mirrors flash at `0x4200_0000` (I-bus) and `0x3C00_0000`
(D-bus) via the XIP cache, so a romfs entry that lives in flash can
serve both as the ELF source (D-bus reads during load) and as the
executable text (I-bus instruction fetch at runtime), with no copy.

## Motivation

| Today | After XIP |
|-------|-----------|
| `ram_text` arena = 128 KB IRAM, every binary copied in | `ram_text` arena dramatically smaller (or gone) |
| Page pool downsized to 24 pages to avoid SRAM1 aliasing | No alias to dodge → page pool can grow toward full `PAGE_COUNT_MAX` |
| CP/M / SOS can't get 17 contiguous pages for Z80 RAM | 17 contiguous pages routinely available |
| `test_cpm` / `test_sos` disabled with `TEST_DISABLED` | Re-enabled, expected to pass (subject to Z80-bridge functional check) |
| Same binary copied at every `execve` (slow + memory churn) | Mapped once at boot, every `execve` is metadata-only |

## Current build artifacts

`scripts/build.sh` already produces three ELF variants per user
binary, all built but not all staged:

| Variant | Linker script | Staged? | Used by |
|---------|---------------|---------|---------|
| `<app>.elf` | `src/arch/xtensa/user/user.ld` | Yes (RAM layout) | Today's loader |
| `<app>.xip.elf` | `src/arch/xtensa/user/user_xip.ld` (PIC, base-relative) | **No** | Diagnostic artifact |
| `<app>.xipfix.elf` | same + `--defsym=__ppap_xip_flash_base=0x3C000000` | **No** | Diagnostic artifact |

The XIP variants put `.literal`, `.text*`, and `.rodata` in one
read-only segment and keep `.got`, `.data`, `.bss` in a separate
writable segment.  That matches the structure the loader will need:
the read-only segment maps directly to flash, the writable segment
allocates in DRAM as usual.

## Open design questions

1. **Which XIP variant to stage?**

   - **PIC (`*.xip.elf`):**  Code is position-independent — the same
     ELF can be placed at any flash offset.  Self-relocating via the
     existing `R_XTENSA_32` / `R_XTENSA_PLT` machinery, which the
     loader already handles.  More work at boot (relocations against
     `text_base = flash_addr_of_segment`), but romfs assembly stays
     straightforward.

   - **Fixed-base (`*.xipfix.elf`):**  Code is pre-linked for a known
     flash address (`0x3C000000`).  Loader does zero text
     relocations.  But: every binary occupies a specific flash slot,
     so the romfs layout has to assign non-overlapping addresses or
     each binary needs its own partition.  Brittle for a romfs with
     dozens of binaries.

   **Recommendation:** PIC.  The relocation cost is one-shot at
   `execve`, the romfs stays a single immutable blob, and the
   existing loader already runs `R_XTENSA_32` over `.rela.text`.

2. **D-bus vs I-bus addressing for the ELF source.**

   The loader needs to *read* the ELF (D-bus, `0x3C00_xxxx`) and
   produce an entry pointer the user process can *execute* through
   (I-bus, `0x4200_xxxx`).  Two aliases of the same flash bytes;
   different cache paths.

   - Convert source pointers from D-bus to I-bus when filling
     `out->entry` and the text-segment base used for relocations:
     `iram = dram - 0x3C000000 + 0x42000000`.
   - Verify both views are mapped by ESP-IDF's default cache
     configuration (`CONFIG_MMU_PAGE_SIZE`, `CONFIG_ESPTOOLPY_FLASHMODE_DIO`).
   - No cache invalidation needed — flash mappings are
     write-through and the I-cache is cold for first access.

3. **Romfs section attributes.**

   `romfs.bin` currently rides in `.rodata.romfs` of the kernel ELF.
   `.rodata` is mapped via the D-bus by ESP-IDF's link map, which is
   fine for reads but **not addressable from the I-bus**.

   Three options:

   - **Place romfs in a dedicated I-bus-mapped section** (e.g.
     `.flash.text` or a custom section the kernel linker script
     routes to a flash region exposed both to D-bus and I-bus).
   - **Use ESP-IDF partition table** to put romfs in a separate
     flash partition that's mapped via the I-bus cache, and have the
     kernel resolve the partition base at boot.
   - **Copy on first use** — pull each binary's read-only segment
     into IRAM at `execve` (same as today, defeats the proposal).

   **Recommendation:** dedicated section.  Smaller surface area than
   the partition route; matches what `pico1calc` does today with its
   romfs in flash.

4. **Relocation arithmetic.**

   For a PIC XIP binary loaded at flash address `F` with data segment
   allocated at SRAM address `D`:

   - Text base for relocations = I-bus alias of `F` (entry point and
     any L32R-referenced text address).
   - Data base for relocations = `D` (RW segment in SRAM).
   - The split-relocation helper `elf_split_addr` already handles
     "link-time address < data_va → text_base" vs "≥ data_va →
     data_base".  Pass it `text_base = (F - 0x3C000000) + 0x42000000`
     and the existing code does the right thing.

## Plan

Phased so each step is independently committable and rollback-able.

### Phase X-1: Loader gating

- Drop the `#if defined(__xtensa__) return ELF_TEXT_SRAM;` short-
  circuit in [elf_loader.c `elf_text_mode`](/src/kernel/core/exec/elf_loader.c).
- Have the caller (`exec_execve` / `flat_loader.c`) pass
  `EXEC_FLAG_XIP_SOURCE` for romfs-resident binaries on xtensa.
- Add the D-bus → I-bus translation in `elf_load_from_buffer` when
  `text_mode == ELF_TEXT_XIP` on xtensa (else the `out->entry`
  already-correct logic stands).

**Exit criterion:** an xtensa user binary loaded from a flash-mapped
buffer launches and returns; verified by a one-shot smoke binary
(simplest "exit(0)" program) before touching the test suite.

### Phase X-2: Romfs in flash

- Add an I-bus-addressable flash section to the xtensa_cc kernel
  linker / IDF component; route `__romfs_start` / `__romfs_end` into
  it.
- Confirm via `xtensa-esp32s3-elf-objdump -h` that the romfs symbols
  land in an `0x42xxxxxx` range (or are reachable from one via the
  cache).

**Exit criterion:** kernel boots, `__romfs_start` resolves through
the I-bus alias, the smoke binary from X-1 still runs.

### Phase X-3: Stage `.xip.elf` into romfs

- Update `scripts/build.sh` xtensa branch to stage `*.xip.elf` into
  `$ROMFS_STAGING/bin/` instead of the RAM variant.
- Add a fallback knob (`XTENSA_USE_XIP=0`) for quick A/B comparison
  during bring-up.
- Update `cmake/stage_romfs.cmake` to recognise the xtensa XIP
  artifacts (or leave the shell-coded staging as-is and unify in a
  later cleanup — see `docs/proposals/cardcomputer_port.md`).

**Exit criterion:** `runtests` boots into a romfs of `.xip.elf`
binaries; every test that passes today still passes.

### Phase X-4: Tune memory budget

- Cut `MEM_REGION_RAM_TEXT_ARENA_SIZE` from 128 KB to ~16 KB (kernel
  callbacks / signal trampolines might still want IRAM; user text no
  longer does).
- Drop or shrink `XTENSA_IDF_HEAP_RESERVE` accordingly — without the
  ram_text arena, the SRAM1 alias check has nothing to defend, and
  IDF heap headroom is naturally larger.
- Re-enable `test_cpm` / `test_sos` in [tests/user/runtests.c](/tests/user/runtests.c)
  and verify the Z80 emulator's contiguous 17-page grab succeeds.

**Exit criterion:** xtensa user-test lane shows ≥ 21 / 21 passing,
including `test_cpm` and `test_sos` (subject to any remaining Z80-
bridge functional bugs).

### Phase X-5: Trim and document

- Remove the `.xipfix.elf` build path if X-1..X-4 settle on the PIC
  variant.
- Update `docs/targets/xtensa.md` to describe the post-XIP arena
  layout (no `ram_text`, larger pool) and retire the SRAM1
  dual-map alias safeguards section.
- Retire this proposal.

## Testing strategy

Each phase ends with a full four-lane test run (`qemu_arm`,
`qemu_m68k`, `qemu_rv32`, `xtensa_cc`).  XIP changes touch only the
xtensa branch of `elf_loader.c` and the xtensa user-binary build, so
the other three lanes should be no-op; verifying each lane catches
accidental sharing.

For xtensa specifically, add or extend kernel tests that probe:

- Flash-resident binary executes through the I-bus alias (no copy
  into IRAM).
- Loader correctly applies D-bus → I-bus translation for the entry
  point.
- Relocations in `.rela.text` resolve against the I-bus text base,
  not the D-bus source pointer.

## Rollback

Each phase is its own commit.  If X-1 lands and a regression
surfaces, revert that single commit; the build pipeline keeps
producing the RAM-layout ELF, so the previous behaviour is intact.

If X-3 lands and the staged `.xip.elf` blob exposes a runtime bug,
the `XTENSA_USE_XIP=0` knob from X-3 keeps a known-good RAM-only
build available without a code revert.

## Out of scope

- **PSRAM execution.**  ESP-IDF's
  `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` / `CONFIG_SPIRAM_RODATA` /
  `CONFIG_SPIRAM_XIP_FROM_PSRAM` give a similar shape (execute from
  PSRAM instead of internal RAM), but CardComputer has no PSRAM.
  A future PSRAM-equipped xtensa target can layer on top of the
  flash-XIP work here — same loader hooks, different `EXEC_FLAG_*`.

- **Romfs staging unification.**  `cardcomputer_port.md` calls out
  the shell-coded xtensa romfs staging as a separate cleanup item.
  This proposal can stay with the existing shell-coded path; the
  unification is independent.

- **Z80 emulator functional bugs.**  If `test_cpm` / `test_sos`
  still fail after X-4 unblocks the memory budget, those failures
  are a separate investigation (CP/M / SOS bridge correctness, not
  the loader path).
