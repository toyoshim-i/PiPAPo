# M5Stack CardComputer Target Support

Device-specific PPAP porting plan for the M5Stack CardComputer
(`xtensa_cc`) target.  Pure hardware facts — block diagram, pin
assignments, ST7789V2 datasheet info, keyboard matrix topology and
keymap, microSD pinout, memory map — live in
[`docs/reference/cardcomputer.md`](../reference/cardcomputer.md).
Xtensa architecture details (ISA, toolchains, memory model, trap
handling, ELF loading) and the PPAP-side design choices that have
already shipped for this target (console layout, display strategy,
keyboard polling / Fn-layer mapping) live in
[`docs/targets/xtensa.md`](../targets/xtensa.md).

This file holds the remaining forward-looking work: phases still
partial or not started, and known gaps that aren't phase-blocking
but should land eventually.

## Goals and Scope

### Primary Goals

1. **Display support** — ST7789V2 framebuffer console (`/dev/tty1`) —
   **done.**
2. **Keyboard support** — GPIO matrix scan providing `TARGET_CAP_KBD`
   input to `/dev/tty1` — **done.**
3. **microSD support** — `TARGET_CAP_SD` for persistent filesystem —
   **not started** (Phase CC-6 below).

### Extended Goals

- Dual-core ESP32-S3 (launch core 1 via `TARGET_CAP_CORE1`).
- USB CDC-ACM as `/dev/ttyUSB0` (second TTY).
- Wi-Fi networking stack (future; would be a major new subsystem).

### Out of Scope

- Bluetooth / BLE support.
- I2S audio / speaker driver.
- IR transmitter.
- PSRAM (not present on STAMP S3).
- Wi-Fi (first port focuses on bare-metal peripherals).

---

## Current Status

| Phase | Status |
|-------|--------|
| CC-3.5: runtime ownership handoff (see breakdown below) | partial |
| CC-6: microSD over HSPI | not started |

User-space boots to working `push` shell prompts on both USJ
(`ttyS0`) and the LCD (`tty1`), with keyboard input, Ctrl-C
delivery, and exec / vfork / signal-action paths exercised on both
consoles.  `target_caps()` advertises
`TARGET_CAP_SPI | TARGET_CAP_DISPLAY | TARGET_CAP_KBD`; `TARGET_CAP_SD`
arrives with CC-6.

### Known Gaps (tracked, not phase-blocking)

- **xtensa_cc romfs staging is shell-coded, not unified with
  `cmake/stage_romfs.cmake`**: `scripts/build.sh` open-codes the
  staging directory layout, ELF install destinations, and
  `/bin/sh→push` symlink for xtensa_cc, while every other target's
  staging goes through `cmake/stage_romfs.cmake`.  The two lists
  drifted at least once (xtensa_cc was missing `/home`, `/usr`,
  `/mnt`).  Right unification: have xtensa_cc invoke
  `cmake -P stage_romfs.cmake` too, paired with extending
  `stage_romfs.cmake` to handle xtensa_cc's `.xip`/`.xipfix`
  artifact ELFs (or dropping them — they're build-time analysis
  artifacts that the runtime loader doesn't yet use).

- **64-bit math helpers not available**: `calc` is currently
  excluded from the xtensa_cc user-app build because it reaches for
  `__udivdi3` / `__ashldi3` etc., and the ESP-IDF toolchain only
  ships a windowed-ABI `libgcc.a` that would corrupt registers when
  called from PPAP's call0 user-space.  Real fix: a small set of
  call0-compatible 64-bit helpers in `src/user/lib/`.

- **Scheduler is semi-preemptive only**: the timer ISR sets
  `xtensa_switch_pending` but the actual context switch happens at
  the next cooperative yield (idle loop or syscall return), not in
  the interrupt-return path.  An earlier debug log captured the
  second yield-to-init crashing with `IllegalInsn` at `retw.n` in
  `xtensa_do_yield` (frame zeroed between save and restore); that
  symptom no longer reproduces during interactive boot but the
  underlying race may still be latent.  True preemptive switching
  in the interrupt return path, plus a focused investigation of
  the saved-frame race, was addressed by the fixed-kstack context-switch
  cleanup.  The steady-state Xtensa switch model is documented in
  [`docs/kernel/context_switch.md`](../kernel/context_switch.md) and
  [`docs/kernel/stack.md`](../kernel/stack.md).

- **User-space loader is RAM-only.**  Every `execve` on xtensa_cc
  copies the user binary's `.text` and `.rodata` into the IRAM
  `ram_text` arena, which forces the 128 KB IRAM rental and the
  SRAM1 dual-map alias safeguards that constrain the DRAM page pool
  to 24 pages.  This is the underlying reason `test_cpm` /
  `test_sos` cannot allocate the 17 contiguous pages a Z80 emulator
  instance needs.  Plan, design, and per-step rollout live in
  [docs/proposals/xtensa_xip.md](xtensa_xip.md).

---

## Phase CC-3.5: Runtime Ownership Handoff

### Guiding Principle

Use ESP-IDF to get the ESP32-S3 into a safe, initialized state, then
shift runtime ownership to PPAP as early as practical.

- ESP-IDF is the bootstrap path for boot ROM integration, clock /
  cache setup, flashing, and vendor-sensitive bring-up.
- PPAP should become the runtime owner of exceptions, scheduling,
  memory layout, protection policy, and board peripherals.

This intentionally moves away from "ESP-IDF as the permanent HAL"
and toward "ESP-IDF as the launch platform."

### Substeps

| Step | Description | Status |
|------|-------------|--------|
| CC-3.5a | Define explicit PPAP-owned memory regions for IRAM text, DRAM user data, kernel DRAM, and device/DMA use | partial |
| CC-3.5b | Replace ad-hoc ESP-IDF heap usage in the Xtensa loader with PPAP region allocators | done |
| CC-3.5c | Move exception / interrupt ownership as fully as possible under PPAP after `app_main()` | partial |
| CC-3.5d | Reintroduce PMS with a PPAP-defined user/kernel memory map | not started |
| CC-3.5e | Prefer direct MMIO drivers for GPIO/SPI/I2C/UART once the bootstrap phase is complete | partial |

### Per-step state (audited against current code)

- **CC-3.5a — partial.** Region allocator abstraction is in place:
  the generic [`src/kernel/core/mm/mem_region.{h,c}`](/src/kernel/core/mm/mem_region.c)
  defines the `PPAP_MEM_RAM_TEXT` / `PPAP_MEM_RAM_DATA` /
  `PPAP_MEM_RAM_STACK` / `PPAP_MEM_DEVICE_DMA` classes, and per-arch
  arena policy lives behind the `mem_helper` hooks. The xtensa overlay
  at [`src/arch/xtensa/kernel/core/mem_helper.c`](/src/arch/xtensa/kernel/core/mem_helper.c)
  requests an IRAM `ram_text` arena (128 KB) plus a DRAM page pool
  (48 × 4 KB = 192 KB requested, downsized to 24 × 4 KB at runtime when
  the SRAM1 dual-mapping alias check rejects pages that physically
  overlap `ram_text`). `PPAP_MEM_RAM_DATA` now routes through the
  shared page pool — no separate `ram_data` arena. What's still
  missing: the arenas are *bootstrapped* through ESP-IDF's
  `heap_caps_malloc()` inside `mem_helper_init_arenas()` /
  `mem_helper_init_pool()`. Closing this means owning the initial
  IRAM/DRAM partitioning from a PPAP-side linker carve-out (or a
  one-shot reservation against `heap_caps_get_largest_free_block`)
  instead of letting ESP-IDF decide the carve-out. The SRAM1 alias
  safeguard would also become unnecessary once the page pool comes
  from a region the linker proves can't alias `ram_text`.

- **CC-3.5b — done.** `src/kernel/core/exec/elf_loader.c` allocates
  text/data/stack via `mem_region_alloc()` exclusively; no direct
  `heap_caps_*` / `malloc` / `calloc` / `free` calls remain in the
  loader path. The remaining `heap_caps_malloc` use is the one-shot
  arena bootstrap noted under CC-3.5a, not the loader.

- **CC-3.5c — partial.** PPAP owns all 30 exception causes
  (`xtensa_ill_handler` for EXCCAUSE=0 syscall trap,
  `xtensa_fault_handler` for EXCCAUSE=1–29) — `xtensa_trap_ready` is
  asserted in `target_late_init()`. PPAP owns CCOMPARE0 (CPU INT 6).
  SYSTIMER (CPU INT 12) is patched to a no-op ISR with the SYSTIMER
  hardware fully gated. Watchdog and brownout detector are *disabled
  via sdkconfig* rather than re-homed under PPAP — acceptable while
  the kernel doesn't need them, but a real handoff would re-enable
  them with PPAP-owned ISRs. USB-JTAG runs polled, so it has no ISR
  to migrate.

- **CC-3.5d — not started.** No PMS register access anywhere in tree.
  `CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=n` in sdkconfig disables
  ESP-IDF's default policy. Blocked on CC-3.5a: a meaningful
  user/kernel memory map needs PPAP-owned regions to point the PMS
  windows at.

- **CC-3.5e — partial.** USB Serial JTAG goes through the ESP-IDF
  HAL `usb_serial_jtag_ll_*` layer-1 wrappers (thin MMIO accessors,
  not the driver framework), so the console is effectively bare-metal
  already. The SPI2 display transport and the SPI3 SD transport
  still ride on ESP-IDF `spi_master`; lowering them to bare MMIO +
  PPAP-owned DMA descriptors is the natural follow-up.

### Suggested next move on CC-3.5

Of the three "partial" rows, the one that unblocks the most
downstream work is **CC-3.5a's bootstrap gap**: owning the initial
IRAM/DRAM carve-out enables CC-3.5d (PMS) and removes the last
ad-hoc ESP-IDF heap dependency. CC-3.5e is naturally driven by
CC-6 as that driver lands. CC-3.5c's remaining items
(watchdog/brownout under PPAP) are low priority while those
subsystems stay disabled.

---

## Phase CC-6: microSD

Hardware reference: [reference §microSD](../reference/cardcomputer.md#microsd).
HSPI (SPI3_HOST), MISO=39, MOSI=14, SCK=40, CS=12.

PPAP-side plan:

- Reuse PPAP's existing `spi_sd.c` driver.  An xtensa_cc-specific
  HSPI transport wrapper similar to `spi_lcd_xtensa_cc.c` will host
  the ESP-IDF `spi_master` glue for SPI3.
- FAT32 read support via the existing `vfat.c` mount path.
- Mount as `/mnt/sd`, set `TARGET_CAP_SD` in `target_caps()`.

### Substeps

| Step | Description |
|------|-------------|
| CC-6a | HSPI transport wrapper at `src/target/xtensa_cc/kernel/vfs/driver/spi_sd_xtensa_cc.c` exporting the existing `spi_sd.h` API; phase-1 uses ESP-IDF `spi_master` (matches CC-4b's choice for SPI2). |
| CC-6b | Wire `spi_sd.c` + `vfat.c` into the ESP-IDF component build; mount `/mnt/sd` from `xtensa_cc_logger.c::vfs_notify(VFS_EVENT_LATE_INIT)` (after the display + keyboard come up). |
| CC-6c | Update `target_caps()` to include `TARGET_CAP_SD`; verify a FAT32-formatted card mounts and reads end-to-end on hardware. |

### Open questions / risks

- **HSPI/VSPI naming.**  ESP32-S3 calls them `SPI2_HOST` / `SPI3_HOST`;
  CC-4b already claims SPI2 for the display.  The SD transport must
  request SPI3 explicitly and use a separate `spi_bus_initialize`
  invocation so the two transports don't collide.
- **Card detect / hot-swap.**  The CardComputer's microSD slot has
  no card-detect line wired (per [reference §microSD](../reference/cardcomputer.md#microsd)).
  First implementation can poll at mount time only; live hot-swap is
  out of scope.
- **DMA buffer placement.**  `spi_master` wants its DMA buffers in
  internal DMA-capable RAM.  Same CC-3.5e shortcut applies — fine
  for bring-up, lower to bare MMIO + PPAP-owned descriptors later.

---

## Cross-cutting Risks

Per-phase risks live under each Phase CC-X section above.  This
table covers risks that span phases or the lifetime of the port:

| Risk | Mitigation |
|------|-----------|
| 512 KB SRAM is tight if anything ever wants a full RGB565 framebuffer (63 KB) | Text-mode cell buffer + scanline streaming keeps fbcon under 1 KB SRAM; never allocate the full framebuffer. |
| Display SPI (SPI2) and microSD SPI (SPI3) on different controllers | Already separate hosts per [reference §Peripheral Pin Assignments](../reference/cardcomputer.md#peripheral-pin-assignments); no bus contention possible. |

---

## References

- [Hardware reference: docs/reference/cardcomputer.md](../reference/cardcomputer.md)
  — block diagram, pinout, ST7789V2 datasheet info, full keymap and
  silkscreen icons, external upstream links (M5Cardputer Arduino
  library, ST7789V2 datasheet, ESP32-S3 TRM).
- [Xtensa architecture details + xtensa_cc design choices](../targets/xtensa.md)
  — memory arenas, SRAM1 alias safeguards, ELF loader direction,
  and the shipped console / display / keyboard design for the
  CardComputer port (§8).
- [XIP rollout plan](xtensa_xip.md) — supersedes the
  RAM-only-loader Known Gap above.
- [Context-switching](../kernel/context_switch.md) and
  [kernel stacks](../kernel/stack.md) — document the fixed-kstack
  scheduler model used by xtensa_cc.
