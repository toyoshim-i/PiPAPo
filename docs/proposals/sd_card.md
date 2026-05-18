# Proposal: SD Card Support on pico1calc and xtensa_cc

> **Status**: Not started.  Generic `spi_sd.c` + `vfat.c` already
> exist and work in QEMU (loopback FAT image over ramblk).  Both
> hardware targets currently boot without a real SD mount.

## Summary

Two distinct targets ship with a physical microSD slot but neither
mounts it:

- **pico1calc** has the `spi_sd.c` driver wired into the build and
  `mmcblk0 /mnt/sd vfat rw` in `/etc/fstab`, but `sd_init()` is
  `#if 0`'d out in `pico1calc_logger.c` because the first PL022
  transfer after UF2 warm boot hangs — TX accepted, no RX bytes
  produced.  `TARGET_CAP_SD` is still advertised optimistically.
- **xtensa_cc** has no SPI3 transport, no `target_caps()` SD bit,
  and no fstab entry.

This proposal covers both: diagnose and unblock the pico1calc PL022
hang, then add the xtensa_cc HSPI transport so the existing
`spi_sd.c` / `vfat.c` stack lights up on both boards.

## Motivation

`spi_sd.c` + `vfat.c` already work end-to-end on `qemu_arm` against
the `ramblk` mock device, so the generic file-system side of the
stack is exercised every CI run.  What's missing is the per-target
SPI transport plus, in pico1calc's case, an investigation of a
real-hardware bring-up bug that's been masked behind `#if 0`.

| Target | Today | After |
|--------|-------|-------|
| `pico1calc` | SD slot present, `TARGET_CAP_SD` set, `sd_init()` disabled with TODO comment | `sd_init()` re-enabled, `/mnt/sd` mounts, FAT32 files visible |
| `xtensa_cc` | No SD transport, `TARGET_CAP_SD` unset, no fstab | HSPI transport wired, `/mnt/sd` mounts, FAT32 files visible |
| `qemu_arm` (existing) | ramblk loopback `/mnt/sd` over FAT image | unchanged (regression guard) |

## Current state (per target)

### pico1calc

- `src/target/pico1calc/CMakeLists.txt` already lists `spi_sd.c` in
  its source list.
- `src/target/pico1calc/kernel/vfs/driver/pico1calc_logger.c:196-213`
  has the `sd_init()` call guarded by `#if 0`, with a comment
  describing the symptom (PL022 accepts TX data, never produces RX
  data) and three hypotheses to investigate (SPI0 loopback, clk_peri
  state, boot2 leaving QSPI pin mux active).
- `src/target/pico1calc/romfs/etc/fstab` already declares
  `mmcblk0  /mnt/sd  vfat  rw`.
- `target_caps()` already returns `TARGET_CAP_SD` (line 53).
- SPI0 is shared with the LCD (ST7365P) which works fine via
  `spi_lcd_rpico.c`, so the bus itself is not dead — `spi_sd.c`
  goes through a different code path (`spi_lcd.h` vs raw
  `spi_xfer()`).

### xtensa_cc

- No SD code in `src/target/xtensa_cc/` at all.
- Hardware spec (HSPI = SPI3_HOST, MISO=39, MOSI=14, SCK=40, CS=12,
  no card-detect line) is in
  [reference §microSD](../reference/cardcomputer.md#microsd).
- `target_caps()` does not advertise `TARGET_CAP_SD`.
- The display SPI2 transport (`spi_lcd_xtensa_cc.c`) already proves
  the ESP-IDF `spi_master` pattern; the SD transport can mirror it
  with `SPI3_HOST` instead of `SPI2_HOST`.

### Generic infrastructure (already in place — no changes needed)

- `src/kernel/vfs/driver/spi_sd.c` — SD command sequence, CSD/CID
  parsing, `mmcblk0` block-device registration via
  `blkdev_register()`.
- `src/kernel/vfs/vfat.c` — FAT32 read/write mount logic, plumbed
  through `vfs_ops_t.mount`.
- `src/kernel/vfs/driver/blkdev.{c,h}` — block-device name registry.
- `mountall` reads `/etc/fstab` and mounts `mmcblk0 → /mnt/sd vfat`
  when both the block device exists and `TARGET_CAP_SD` is set.

## Open design questions

1. **What's actually wrong with pico1calc SPI0?**

   Three candidate causes from the existing TODO comment, in
   decreasing order of likelihood per the hardware-bring-up
   experience already in this repo:

   - **boot2 leaves QSPI pin mux active on flash GPIOs**, and SPI0
     happens to share one or more of those pins.  The Pico SDK's
     `boot2_w25q080` reconfigures GPIO0..3 for QSPI; warm boot
     wouldn't restore PL022 pin function.  Worth confirming by
     reading the GPIO function-select registers right before the
     first `spi_xfer()` and comparing against cold-boot values.
   - **clk_peri stopped or wrong frequency.**  Easy to rule in/out
     by reading `CLOCKS_CLK_PERI_CTRL` and `CLOCKS_FC0_RESULT`
     before the hung transfer.
   - **PL022 stuck in a previous-transfer wait state** because the
     LCD path leaves something half-finished on warm boot.  Less
     likely (LCD path works), but worth checking the SSPSR.BSY bit
     before issuing the SD probe.

   The investigation has to be data-driven — speculating without
   GPIO/clk register dumps will burn time the same way the original
   TODO did.

2. **Hot-plug / card-detect.**

   - pico1calc: physical card-detect switch present?  (Check
     schematic.)  If wired, route it to a GPIO and poll at
     `VFS_EVENT_IDLE` to lazy-mount.  If not, mount-at-boot only is
     the same constraint as xtensa_cc.
   - xtensa_cc: no card-detect line ([reference
     §microSD](../reference/cardcomputer.md#microsd)).  Mount-at-boot
     only.  Live hot-swap is out of scope.

   **Recommendation:** mount-at-boot only for both targets in v1.
   Hot-plug is a separate proposal.

3. **CS / DC pin sharing on pico1calc.**

   Display CS, backlight, and SD CS all sit on SPI0 / nearby GPIOs.
   Verify that `sd_init()` doesn't accidentally hold the display CS
   asserted (or vice versa) during the probe sequence.  Mitigation
   is the standard SPI-bus-mutex pattern; `spi_sd.c` already
   manages CS via its own `cs_assert`/`cs_release` helpers.

4. **DMA buffer placement on xtensa_cc.**

   ESP-IDF `spi_master` wants its DMA buffers in internal
   DMA-capable RAM.  Same CC-3.5e shortcut already applied to the
   display transport — acceptable for bring-up, lower to bare MMIO +
   PPAP-owned DMA descriptors later as a CC-3.5e follow-up.

## Plan

Phased so each sub-goal is independently committable and rollback-
able.  Sub-goal A (pico1calc) is sequenced first because it
exercises the existing wiring end-to-end on hardware, smoking out
any bugs in the shared `spi_sd.c` / `vfat.c` stack before xtensa_cc
adds a second transport on top.

### Sub-goal A: pico1calc re-enable

| Step | Description | Exit criterion |
|------|-------------|----------------|
| A-1 | Add a diagnostic dump at the top of `sd_init()` (or a one-shot ktest) that reads `IO_BANK0_GPIOx_CTRL` for the SPI0 pins, `CLOCKS_CLK_PERI_CTRL`, `CLOCKS_FC0_RESULT` after a frequency-count run, and `SSPSR` for SPI0.  Run on hardware after cold boot and after UF2 warm boot; compare.  No code-path change yet. | Concrete numbers for the three hypotheses; root cause identified. |
| A-2 | Apply the fix the data points to (most likely: re-assert PL022 pin mux on GPIOs SPI0 shares with the QSPI/flash boot2 path).  Keep `sd_init()` still `#if 0`'d for the diagnostic commit. | Diagnostic from A-1 now shows healthy register state on both boots. |
| A-3 | Remove the `#if 0`, drop the TODO comment, plumb the `sd_init()` rc through `klogf` (the existing call site already does this).  No fstab change needed. | `boot` log shows `SD: card initialised, mmcblk0 registered`; `mount` lists `/mnt/sd`; `ls /mnt/sd` returns FAT32 file list. |
| A-4 | Add a hardware smoke check (read a known file, verify contents) and a deliberate no-card boot path (`-ENODEV` path already exists in `sd_init()` — verify the log says "no card detected" and the kernel continues). | Both card-present and card-absent boots succeed; no crash, no hang. |

### Sub-goal B: xtensa_cc wire-up

| Step | Description | Exit criterion |
|------|-------------|----------------|
| B-1 | Add `src/target/xtensa_cc/kernel/vfs/driver/spi_sd_xtensa_cc.c` — HSPI transport wrapper exporting the existing `spi_sd.h` low-level primitives (cs / xfer / 400 kHz init → 25 MHz run).  Phase-1 uses ESP-IDF `spi_master` with `SPI3_HOST` (matches the display transport's choice of `SPI2_HOST` for SPI2).  Wire `spi_sd.c` and `vfat.c` into the ESP-IDF component build. | Standalone ktest sends CMD0 / CMD8 / ACMD41 over HSPI and gets the expected responses from a card. |
| B-2 | Add `mmcblk0  /mnt/sd  vfat  rw` to xtensa_cc's romfs `/etc/fstab` (mirrors pico1calc).  Call `sd_init()` from `xtensa_cc_logger.c::vfs_notify(VFS_EVENT_LATE_INIT)` after display + keyboard come up.  Update `target_caps()` to OR in `TARGET_CAP_SD`. | Boot log shows `SD: card initialised`; `mount` lists `/mnt/sd`; `ls /mnt/sd` returns FAT32 file list on hardware. |
| B-3 | No-card boot path: verify `sd_init()` returns `-ENODEV` cleanly and the kernel boots without `/mnt/sd`.  `mountall` should skip the fstab entry per the existing `TARGET_CAP_SD` + blkdev-presence gate. | Both card-present and card-absent boots succeed. |

### Sub-goal C: documentation cleanup

- Update `docs/targets/xtensa.md` §8 with the SD bring-up notes
  (HSPI host choice, no card-detect, mount-at-boot only).
- Retire this proposal once A-3 and B-2 are green on hardware for
  one full release cycle.

## Testing strategy

- **QEMU (no regression):** `qemu_arm` already mounts a loopback
  FAT image through `ramblk` / `vfat.c`; that lane must stay green
  on every commit, since it exercises the generic file-system
  layer that both sub-goals depend on.
- **pico1calc hardware:** the only place sub-goal A can be
  validated.  The diagnostic in A-1 needs cold boot + warm boot
  data points, both with and without an SD card inserted.
- **xtensa_cc hardware:** the only place sub-goal B can be
  validated.  Same matrix (cold/warm, card present/absent).

## Rollback

Each step is its own commit.

- **Sub-goal A** regressions: revert the A-2/A-3 commits; the
  `#if 0` guard returns and the board boots as it does today
  (without SD).
- **Sub-goal B** regressions on xtensa_cc: revert the B-2 commit
  (transport stays in tree but stops being initialised); board
  boots without SD as it does today.

## Out of scope

- **Hot-plug / live remount.**  Both targets ship as
  mount-at-boot only.  A future card-detect proposal can wire the
  GPIO + add a re-probe trigger.
- **SDHC / SDXC beyond what `spi_sd.c` already supports.**  The
  existing driver speaks SPI mode with CMD8 / ACMD41 / CMD58 and
  decodes the CSD v1/v2 capacity fields; anything beyond that is
  a generic-driver enhancement, not a per-target one.
- **Write performance tuning.**  First cut is correctness, not
  throughput.  Block caching, multi-sector writes, and TRIM are
  follow-up work once both boards are mounting reliably.
- **Lowering xtensa_cc HSPI to bare MMIO.**  CC-3.5e (xtensa.md
  §3 "ESP-IDF Integration") tracks the broader "lower the SPI
  transports to bare MMIO + PPAP-owned DMA" work; the SD
  transport will be one of its consumers, not its driver.
