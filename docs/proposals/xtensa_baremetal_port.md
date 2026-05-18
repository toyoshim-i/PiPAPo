# Proposal: Bare-metal Xtensa LX7 Port

> **Status**: Draft, not started.  The current
> [`xtensa_cc`](../targets/xtensa.md) target boots via ESP-IDF as
> "ESP-IDF as launch platform"; ESP-IDF still owns boot ROM
> integration, cache / MMU setup, clock tree, PMS, watchdog, and the
> SPI / heap / flash HALs.  This proposal scopes a future port that
> removes ESP-IDF entirely and lets PPAP own boot end-to-end.

## Summary

The current xtensa_cc port intentionally stops short of full
runtime-ownership handoff: ESP-IDF stays as the bootstrap *and*
runtime HAL.  That is the right call for the first Xtensa target
because it lets
PPAP focus on architecture work (context switch, trap handling,
ELF loading) without also reimplementing every ESP-IDF boot step.

This proposal sketches the follow-up: a separate target (working
name `xtensa_bm`, first board the M5Stack CardComputer) that
replaces ESP-IDF with a PPAP-owned boot path.  The existing
xtensa_cc port becomes a working reference for the parts that
*don't* change — the kernel core, the user-space ELF loader, the
trap / syscall plumbing.

## Motivation

| Today (xtensa_cc) | After (xtensa_bm) |
|-------------------|-------------------|
| ESP-IDF owns initial IRAM/DRAM partition; PPAP arenas bootstrap via `heap_caps_malloc` | PPAP linker script owns the partition end-to-end; `mem_helper_init_arenas` is a no-op |
| `CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=n` — no PMS protection | PPAP-defined user/kernel memory map programmed into PMS at `target_late_init` |
| Watchdog / brownout disabled via sdkconfig | PPAP-owned ISRs, real protection |
| SPI2 (LCD), SPI3 (SD) on ESP-IDF `spi_master` | Bare-MMIO SPI driver + PPAP-owned DMA descriptors |
| Flash reads through ESP-IDF SPI flash component | Bare-MMIO flash driver (read-only sufficient for boot) |
| ESP-IDF Docker image (~3 GB) + IDF Python toolchain in CI | Plain `xtensa-esp32s3-elf-gcc` + a small image-packer tool |
| `idf.py build` orchestrates the build | `cmake` direct, same as every other PPAP target |
| Build artifact = ESP-IDF app binary loaded by ESP-IDF bootloader | Build artifact = single image consumed by either the ESP32-S3 mask ROM or a minimal PPAP stage1 |

The CC-3.5 substeps (linker carve-out, PMS, watchdog / brownout
ISRs, bare-MMIO SPI) all land naturally as part of this port,
because there is no ESP-IDF fallback to defer them to.

## What changes vs what stays

### Reuses xtensa_cc verbatim

- `src/arch/xtensa/kernel/core/` — context switch (`switch.S`),
  trap entry, fault handlers, ILL-syscall dispatch.
- `src/arch/xtensa/kernel/core/mem_helper.c` — arena hooks (the
  bootstrap *body* changes; the interface is unchanged).
- `src/arch/xtensa/user/` — call0 ABI user-space, PIC linker
  scripts, XIP linker scripts (post-[xtensa_xip.md](xtensa_xip.md)).
- All of `src/kernel/` — VFS, scheduler, exec, signals, etc.
- ELF / XIP loader.

### Replaced by PPAP-owned code

| ESP-IDF responsibility | PPAP-owned replacement |
|------------------------|------------------------|
| Second-stage bootloader (cache + MMU init, segment loading) | `src/arch/xtensa/boot/stage1.S` + minimal C |
| Clock tree init (XTAL → PLL → CPU/AHB/peripheral) | `src/arch/xtensa/kernel/core/clock_init.c` |
| GPIO / IO_MUX direct register access | `src/target/<board>/kernel/core/gpio.c` (mostly already MMIO-style) |
| USB Serial JTAG | Already through `usb_serial_jtag_ll_*` thin wrappers — promote to direct MMIO |
| SPI master | New `src/arch/xtensa/kernel/vfs/driver/spi_xtensa.c` |
| Flash read (for XIP / romfs) | New `src/arch/xtensa/kernel/core/flash.c` |
| PMS / memory protection | New `src/arch/xtensa/kernel/core/pms.c` |
| Watchdog / brownout | New `src/arch/xtensa/kernel/core/wdt.c` (or stay disabled) |
| `heap_caps_malloc` for arena bootstrap | Linker-script symbols (`__iram_arena_start`, `__dram_pool_start`, …) consumed directly by `mem_helper_init_arenas` |

### New build artifacts

- Image-packer tool (Python or C) that wraps the linked ELF in the
  format the ESP32-S3 mask ROM expects.  ESP-IDF's `esptool.py`
  documents the layout — reimplement the subset PPAP needs
  (single-segment image, no SHA, no encrypted boot).
- `xtensa_bm` target directory: linker script with full
  IRAM/DRAM/flash partition, board-specific glue (display, kbd,
  SD), `target_caps`.
- Optional minimal PPAP stage1 that lives at the ROM-expected
  flash offset and chain-loads the kernel — same shape as
  `src/arch/arm_m/boot/stage1.S` on RP2040.

## Open design questions

1. **Keep the ESP-IDF *bootloader* or replace it?**

   - **Replace.**  Maximum ownership; matches the proposal title.
     Cost: re-implement cache / MMU init and the image format the
     ROM expects.  ESP32-S3 ROM is documented but nontrivial; the
     first cut spends most of its time here.
   - **Keep the IDF stage-1 bootloader (28 KB), drop everything
     above it.**  ESP-IDF's `bootloader_main.c` already does
     cache/MMU init and segment loading.  Treat it as opaque
     firmware; the *app* is fully PPAP-owned with no ESP-IDF
     runtime.  Cost: still depends on IDF for the bootloader
     blob, but the build can pre-build that blob once and check
     it in (or fetch a pinned binary release).

   **Recommendation:** start by replacing.  Defer to "keep IDF
   bootloader" only if the ROM image format proves to be a
   multi-week investigation.

2. **Toolchain.**

   The xtensa_cc build already uses
   `xtensa-esp32s3-elf-gcc` directly for PPAP code (only IDF's
   build system orchestrates it).  Bare-metal can use the same
   compiler — what disappears is `idf.py`, the IDF CMake glue, and
   the IDF Python venv.  No new compiler needed.

3. **First board.**

   CardComputer (`xtensa_bm` first instance shares the existing
   `xtensa_cc` board glue: ST7789V2 driver, keyboard scanner, SD
   transport).  Driver code is target-local under
   `src/target/<board>/` so it ports unchanged; what changes is
   only the transport layer below it (bare-MMIO SPI vs `spi_master`).

4. **Flash layout.**

   ESP-IDF's partition table conflates "where the bootloader
   lives" / "where the app lives" / "where NVS lives".  PPAP only
   needs (a) a place for the image the ROM loads and (b) a place
   for romfs.  Define a minimal layout in the linker script; no
   partition table file required.

5. **Side-by-side build with xtensa_cc?**

   Both targets should remain buildable from `main` for as long as
   `xtensa_cc` is in service.  They share `src/arch/xtensa/`
   entirely and differ only in `src/target/`, sdkconfig presence,
   and the top-level CMake glue.  No conflict.

## Plan

Phased.  Each phase is independently committable and the build
boots into a working REPL by the end of each one (smaller surface
than the previous on every phase — the user-visible surface stays
flat, what changes is *who* implements each layer).

### Phase B-1: Skeleton target + IDF bootloader keep

- Add `src/target/xtensa_bm/` with a minimal `target_caps()`
  and a kernel-only build (no display, no kbd, no SD yet).
- Keep ESP-IDF's stage-1 bootloader as a checked-in binary; PPAP
  produces only the app image.
- Build path: `cmake --preset xtensa_bm` straight, no `idf.py`.
- USJ console via direct MMIO (promote the existing `_ll_*` use to
  raw register writes).

**Exit criterion:** kernel boots to login on USJ; no ESP-IDF
runtime symbols in the linked image (`nm`-check in CI).

### Phase B-2: PPAP-owned bootloader

- Implement `src/arch/xtensa/boot/stage1.S` + cache/MMU init in C.
- Implement the ROM image-format packer; verify the produced
  image matches ESP-IDF's byte-for-byte on a sample.
- Replace the IDF bootloader blob.

**Exit criterion:** kernel boots from a flash image produced
entirely by PPAP build, with no IDF artifact in the tree.

### Phase B-3: Memory ownership

- Linker script owns the full IRAM/DRAM partition.
- `mem_helper_init_arenas` switches from `heap_caps_malloc` to
  consuming `__iram_arena_start` / `__dram_pool_start` linker
  symbols.
- Retire the SRAM1 dual-mapping alias safeguard — no longer needed
  because the linker proves disjointness.

**Exit criterion:** `heap_caps_*` calls are absent from the
xtensa_bm build; `MEM_REGION_RAM_TEXT_ARENA_SIZE` matches the
linker carve-out exactly.

### Phase B-4: Peripherals on bare MMIO

- Direct GPIO + IO_MUX from `src/target/xtensa_bm/`.
- Bare-MMIO SPI (display + SD).
- Bare-MMIO flash read (XIP + romfs source).

**Exit criterion:** display, keyboard, SD all work on hardware;
no ESP-IDF driver symbols linked.

### Phase B-5: Memory protection + watchdog

- PMS programmed at `target_late_init` against the linker carve-out
  from B-3.
- Watchdog ISR wired (RWDT timeout → `panic`).
- Brownout ISR wired (BOD → graceful shutdown if practical, else
  `panic`).

**Exit criterion:** user-space cannot read kernel memory; watchdog
triggers on a deliberate infinite-loop syscall handler test.

### Phase B-6: Retire xtensa_cc?

Open question.  Options:

- **Keep both.**  xtensa_cc stays as the lightweight "use ESP-IDF
  to bring up a new board fast" template; xtensa_bm is the
  production target.
- **Retire xtensa_cc** once xtensa_bm has been stable on the
  CardComputer for one release cycle.

Decide closer to the time — the answer depends on whether anyone
is using xtensa_cc as a bring-up template for new Xtensa boards.

## Testing strategy

- xtensa_bm gets its own QEMU lane if and only if QEMU's
  `esp32s3` machine matures enough to host PPAP.  Until then,
  xtensa_bm is hardware-only.
- xtensa_cc keeps shipping every commit (regression guard for the
  Xtensa arch code that both targets share).
- A nightly hardware soak (display + kbd + SD + USJ) is the only
  practical regression catch for the bare-metal-specific bits
  (boot, clocks, PMS, watchdog).

## Rollback

Each phase is its own commit.  Because xtensa_bm is a separate
target, none of the xtensa_cc users are affected by an in-progress
bare-metal port; a B-N regression simply means "boot xtensa_cc
instead" until the bug is fixed.

If B-6 retires xtensa_cc, that's the only irreversible step — and
it lands behind a release-cycle gate.

## Out of scope

- **Other Xtensa LX7 boards.**  The proposal is scoped to
  CardComputer first.  Once `src/arch/xtensa/boot/` is in place
  the marginal cost of adding another LX7 board is a new
  `src/target/<board>/` directory.
- **Wi-Fi / BT.**  ESP-IDF's networking stack is the only mature
  source for these on ESP32-S3.  A bare-metal Wi-Fi stack is not
  in PPAP's scope.
- **OTA updates.**  Out of scope for v1.  The bare-metal image is
  flashed via the standard `esptool.py write_flash` (the ROM
  bootloader supports it independent of any ESP-IDF runtime).
- **Encrypted boot / secure boot.**  Out of scope.
- **PSRAM execution.**  CardComputer has no PSRAM; revisit when a
  PSRAM-equipped Xtensa board is on the roadmap.
