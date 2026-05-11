# M5Stack CardComputer Target Support

Device-specific plan for the M5Stack CardComputer (`xtensa_cc`) target.
For Xtensa architecture details (ISA, toolchains, memory model, trap handling,
ELF loading), see [`docs/targets/xtensa.md`](../targets/xtensa.md).

---

## 1. Hardware Overview

### 1.1 Block Diagram

```
+-----------------------------------------------+
|  M5Stack CardComputer                          |
|                                                |
|  +------------------+   +------------------+   |
|  | STAMP S3 Module  |   | ST7789V2 Display |   |
|  | (ESP32-S3-FN8)   |-->| 240x135 IPS      |   |
|  | 8 MB Flash (QIO) |   | SPI interface     |   |
|  | 512 KB SRAM      |   +------------------+   |
|  | Wi-Fi + BLE 5.0  |                          |
|  +------------------+   +------------------+   |
|         |               | 56-key Keyboard  |   |
|         +-------------->| GPIO matrix scan |   |
|         |               +------------------+   |
|         |                                      |
|         +---> microSD slot (SPI)               |
|         +---> I2S speaker (NS4168)             |
|         +---> IR transmitter (GPIO44)          |
|         +---> USB-C (native USB + UART)        |
|         +---> Grove port (I2C)                 |
+-----------------------------------------------+
```

### 1.2 Peripherals and Pin Assignments

| Peripheral | Interface | Pins | Notes |
|-----------|-----------|------|-------|
| ST7789V2 display | SPI2 | MOSI=35, SCK=36, CS=37, DC=34, RST=33, BL=38 | 240x135, 65K colors |
| Keyboard | GPIO matrix | Directly from ESP32-S3 GPIOs | 7 rows x 8 columns |
| microSD | SPI (HSPI) | MISO=39, MOSI=14, SCK=40, CS=12 | FAT32 |
| Speaker | I2S | BCLK=41, LRCK=43, DIN=42 | NS4168 amplifier |
| IR TX | GPIO | GPIO44 | 38 kHz modulation |
| USB | Native USB | GPIO19 (D-), GPIO20 (D+) | USB Serial JTAG |
| UART0 | UART | TX=43, RX=44 | **Unusable as console** — pins shared with I2S/IR |

### 1.3 Console Strategy

- **Primary console (`ttyS0`)**: USB Serial JTAG via ESP32-S3's native
  USB (appears as `/dev/ttyACM0` on host).  Used for development and
  flashing.  USJ is the only practical console interface because
  UART0's TX/RX pins are reused for I2S and IR on this board.
- **Display console (`tty1`)** *(future)*: ST7789 + keyboard — joins
  as `KLOG_LOGGER_SECONDARY` on top of the existing USJ primary,
  matching the pico1calc UART+FBCON pattern.
- Default console: `ttyS0` today; flips to `tty1` once display +
  keyboard land and the kernel auto-detects them via target caps.

---

## 2. Goals and Scope

### 2.1 Primary Goals

1. **Display support** — ST7789V2 framebuffer console (`/dev/tty1`) using
   the existing `TARGET_CAP_DISPLAY` + `tty_backend_t` infrastructure.
2. **Keyboard support** — GPIO matrix scan providing `TARGET_CAP_KBD`
   input to `/dev/tty1`, making the CardComputer a standalone terminal.
3. **microSD support** — `TARGET_CAP_SD` for persistent filesystem.

### 2.2 Extended Goals

- Dual-core ESP32-S3 (launch core 1 via `TARGET_CAP_CORE1`).
- USB CDC-ACM as `/dev/ttyUSB0` (second TTY).
- Wi-Fi networking stack (future; would be a major new subsystem).

### 2.3 Out of Scope

- Bluetooth / BLE support.
- I2S audio / speaker driver.
- IR transmitter.
- PSRAM (not present on STAMP S3).
- Wi-Fi (first port focuses on bare-metal peripherals).

---

## 3. Target Layer: `src/target/xtensa_cc/`

### 3.1 File Inventory

| File | Purpose |
|------|---------|
| `xtensa_cc.h` | Pin definitions, clock frequencies, display parameters |
| `target_xtensa_cc.c` | target_early_init/late_init, UART |
| `st7789.c` | ST7789V2 SPI display driver (framebuffer → SPI DMA) |
| `keyboard.c` | GPIO matrix scanner, keymap, key-repeat logic |
| `CMakeLists.txt` | Build configuration (ESP-IDF component) |

### 3.2 `target_caps()`

```c
uint32_t target_caps(void) {
    return TARGET_CAP_SPI
         | TARGET_CAP_DISPLAY
         | TARGET_CAP_KBD;
    /* TARGET_CAP_SD added later when microSD driver lands */
}
```

---

## 4. Display Driver (ST7789V2)

The ST7789V2 is driven over SPI2 at up to 80 MHz:

- **Resolution**: 240x135 pixels, 16-bit RGB565
- **Framebuffer size**: 240 x 135 x 2 = 64,800 bytes (~63 KB)
- **Strategy**: maintain a text-mode buffer (similar to pico1calc),
  render glyphs to the SPI framebuffer, flush dirty regions every 20 ms
  via `sched_set_display_poll()`.
- **Initialization**: SPI2 setup → ST7789 reset sequence → set rotation
  → clear screen → backlight on.
- **TTY backend**: register `tty_backend_t` with putc (glyph render) and
  flush (SPI DMA transfer) callbacks.

With an 8x8 font, the 240x135 display provides a **30x16 character**
terminal — small but usable for a UNIX shell.

### 4.1 SPI Configuration

| Parameter | Value |
|-----------|-------|
| SPI host | SPI2_HOST |
| Clock | 80 MHz |
| MOSI | GPIO35 |
| SCK | GPIO36 |
| CS | GPIO37 |
| DC | GPIO34 |
| RST | GPIO33 |
| Backlight | GPIO38 |

### 4.2 Init Sequence

1. Assert RST low (10 ms), release
2. Send `SLPOUT` (0x11), wait 120 ms
3. `COLMOD` (0x3A) = 0x55 (16-bit RGB565)
4. `MADCTL` (0x36) = rotation setting for landscape
5. `CASET` / `RASET` to set window (0,0)-(239,134)
6. `DISPON` (0x29)
7. Backlight PWM on GPIO38

### 4.3 SRAM Budget

With 512 KB total SRAM, the 63 KB framebuffer is significant (~12%).
Mitigation strategies:

- **Text-mode buffer only** (~2 KB): store character+attribute grid,
  render glyphs on-the-fly during SPI flush. Avoids the 63 KB
  framebuffer entirely.
- **Dirty-rect flushing**: only transfer changed character cells via SPI
  DMA. Reduces SPI bus time from ~6.5 ms (full frame) to typically
  <1 ms.
- **IRAM vs DRAM tradeoff**: framebuffer (if used) goes in DRAM; user
  code goes in IRAM. These don't compete since they're on different
  buses (see [xtensa.md](../targets/xtensa.md) §4).

---

## 5. Keyboard Driver

The CardComputer's keyboard is a 7x8 GPIO matrix:

- **Scan**: drive each row low in sequence, read column GPIOs to detect
  key presses. Debounce with 10 ms delay.
- **Keymap**: ASCII mapping with Fn/Shift modifiers for symbols and
  control characters.
- **Integration**: polled from the display poll callback (every 20 ms),
  feeds characters into the TTY1 input ring buffer via
  `tty_backend_t.getc`.
- **Special keys**: Fn+key combos for Ctrl-C, Ctrl-D, Ctrl-Z, arrow
  keys (VT100 escape sequences).

### 5.1 GPIO Matrix Pinout

The exact row/column pin assignments need to be determined from the
M5Stack Arduino library source (`Cardputer.h` / `Keyboard.cpp`). The
scanning logic is straightforward once pins are known.

---

## 6. microSD Card

| Parameter | Value |
|-----------|-------|
| SPI host | HSPI (SPI3_HOST) |
| MISO | GPIO39 |
| MOSI | GPIO14 |
| SCK | GPIO40 |
| CS | GPIO12 |

- Reuse PPAP's existing SD SPI driver pattern (from pico1calc).
- FAT32 read support for loading programs from SD.
- Mount as `/mnt/sd`, set `TARGET_CAP_SD`.

---

## 7. Implementation Plan

### Current Status

| Phase | Status |
|-------|--------|
| CC-1: boot, clock, BSS, kmain handoff from app_main | DONE |
| CC-2: SysTimer-driven scheduler tick | DONE |
| CC-3: ILL-syscall trap, exec, vfork, signals (delivery still stubbed) | DONE |
| CC-3.1: USB Serial JTAG primary console (TX + RX) | DONE |
| CC-3.5: runtime ownership handoff (see breakdown below) | partial |
| CC-4: ST7789 display + framebuffer console | not started |
| CC-5: GPIO-matrix keyboard + `tty1` input | not started |
| CC-6: microSD over HSPI | not started |

User-space currently boots to a working `push` shell prompt over USJ;
keystrokes are delivered, exec/vfork/signal-action paths are exercised.

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
  the saved-frame race, is the Xtensa scheduler-stability work
  tracked in
  [`docs/proposals/context_switch_cleanup.md`](context_switch_cleanup.md)
  Phase 4 (Migrate Xtensa).

- **User-space loader is RAM-only; tight pool causes OOM cascades
  during the on-target test suite**: every `exec` on xtensa_cc
  copies the user binary's `.text` + `.rodata` + `.data` + `.bss`
  + stack out of the romfs into the page pool, because the runtime
  loader does not yet consume the `.xip` / `.xipfix` ELF variants
  that `scripts/build.sh` already builds.  At boot `/proc/meminfo`
  shows ~72 KB free (≈18 pages); the per-test-binary footprint plus
  fragmentation across sequential `exec` / `exit` cycles causes
  random `MM: OOM: page_alloc failed` failures partway through
  `tests/user/runtests.c`.  This is a loader/memory issue, not a
  test bug — `runtests.c` keeps the affected tests `TEST_ENABLED`
  on `__xtensa__` so they recover automatically once XIP is wired.
  Right fix: extend the user ELF loader to map `.text` / `.rodata`
  segments directly out of flash (or PSRAM) using the existing
  `src/arch/xtensa/user/user_xip.ld` layout, paired with stripping
  any unused `.xipfix` fixed-address variants once the dynamic XIP
  variant is proven.  See the matching note in
  `docs/getting_started/testing.md` "Known coverage gaps" for the
  per-test breakdown.

### Guiding Principle

Use ESP-IDF to get the ESP32-S3 into a safe, initialized state, then shift
runtime ownership to PPAP as early as practical.

In other words:

- ESP-IDF is the bootstrap path for boot ROM integration, clock/cache setup,
  flashing, and vendor-sensitive bring-up
- PPAP should become the runtime owner of exceptions, scheduling, memory
  layout, protection policy, and board peripherals

This plan intentionally moves away from "ESP-IDF as the permanent HAL" and
toward "ESP-IDF as the launch platform."

### Phase CC-3.5: Runtime Ownership Handoff

| Step | Description | Status |
|------|-------------|--------|
| CC-3.5a | Define explicit PPAP-owned memory regions for IRAM text, DRAM user data, kernel DRAM, and device/DMA use | partial |
| CC-3.5b | Replace ad-hoc ESP-IDF heap usage in the Xtensa loader with PPAP region allocators | done |
| CC-3.5c | Move exception / interrupt ownership as fully as possible under PPAP after `app_main()` | partial |
| CC-3.5d | Reintroduce PMS with a PPAP-defined user/kernel memory map | not started |
| CC-3.5e | Prefer direct MMIO drivers for GPIO/SPI/I2C/UART once the bootstrap phase is complete | partial |

**Per-step state (audited against current code):**

- **CC-3.5a — partial.** Region allocator abstraction is in place:
  `src/kernel/core/mm/mem_region.{h,c}` defines `PPAP_MEM_RAM_TEXT`,
  `PPAP_MEM_RAM_DATA`, `PPAP_MEM_RAM_STACK`, `PPAP_MEM_DEVICE_DMA`
  classes with per-arena allocators (IRAM 64 KB, DRAM 128 KB, plus
  PSRAM placeholders). What's missing: the arenas themselves are still
  *bootstrapped* through ESP-IDF's `heap_caps_malloc()` inside
  `mem_region_init()`. Closing this means owning the initial
  IRAM/DRAM partitioning from a PPAP-side linker script (or a
  one-shot reservation against `heap_caps_get_largest_free_block`)
  instead of letting ESP-IDF decide the carve-out.

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
  already. GPIO / SPI / I2C / UART drivers don't exist yet — they
  arrive with CC-4 (display SPI) and CC-6 (SD SPI), and CC-4b
  explicitly calls out that the first-cut SPI2 transport will use
  ESP-IDF `spi_master` and only later be lowered to MMIO under this
  step.

**Suggested next move on CC-3.5.** Of the three "partial" rows, the
one that unblocks the most downstream work is **CC-3.5a's bootstrap
gap**: owning the initial IRAM/DRAM carve-out enables CC-3.5d (PMS)
and removes the last ad-hoc ESP-IDF heap dependency. CC-3.5e is
naturally driven by CC-4/CC-6 as those drivers land. CC-3.5c's
remaining items (watchdog/brownout under PPAP) are low priority while
those subsystems stay disabled.

### Phase CC-4: Display

The PicoCalc port already proves out this layering; CC-4 reuses it
end-to-end and only adds the parts that are genuinely board-specific
(ST7789V2 init, ESP32-S3 SPI2 transport, 240×135 geometry).

**Reuse map (pico1calc → xtensa_cc):**

| Layer | pico1calc artifact | xtensa_cc plan |
|-------|--------------------|----------------|
| Text console / VT100 / dirty-flush | `src/kernel/vfs/driver/fbcon.c` | reuse as-is |
| Glyph data | `font8x8.c` (generated) | reuse + add font generation to xtensa_cc CMake |
| LCD controller init | `src/kernel/vfs/driver/lcd_panel.c` (ST7365P) | rename existing file to `lcd_st7365p.c`; add new sibling `lcd_st7789.c`. `lcd_panel.h` stays as the generic contract (`lcd_init` / `lcd_fill_rect`). |
| LCD geometry constants | `LCD_WIDTH`/`LCD_HEIGHT` macros in `lcd_panel.h` | move to a per-target header (or compile-time `-D`) so fbcon picks the right grid |
| SPI transport contract | `src/kernel/vfs/driver/spi_lcd.h` | reuse the contract verbatim (cmd / data / data16 / set_window / fill / stream_*) |
| SPI transport impl | `src/arch/arm_m/.../spi_lcd_rpico.c` (PL022) | new `src/target/xtensa_cc/kernel/vfs/driver/spi_lcd_xtensa_cc.c` — start on ESP-IDF `spi_master`, lower to bare MMIO at SPI2 base in CC-3.5e |
| Target glue (init order, klog secondary, tty backend, idle flush) | `pico1calc_logger.c` `vfs_notify()` | mirror in `xtensa_cc_logger.c` |

**Geometry note.** 240×135 with the existing 8×8 IchigoJam font gives a
30×16 grid (240×128, 7 px vertical slack). Existing fbcon `MODE_SQUARE`
already uses the 8×8 font — a small constant tweak (cols/rows for the
new geometry), not a new code path. SRAM cost: cell_char + cell_attr =
2 × 30 × 16 ≈ 960 B; one scanline = 240 × 2 = 480 B on stack during
flush. No full framebuffer required.

**CC-4 is not blocked by CC-3.5.** The display path is kernel-side and
ISR-free (flush polled from the idle loop, which already runs to
service USJ). Per-target cell buffers are static and tiny. PMS isn't
needed. CC-4 itself is the consumer that will eventually drive
CC-3.5e (lower SPI2 from `spi_master` to bare MMIO).

**Prerequisites to handle inside CC-4 (not blockers, but must be
addressed for the build to succeed):**

1. **`.iobuf` section attribute is RP2040-specific.**
   `fbcon.c:53–56` tags `cell_char` / `cell_attr` with
   `__attribute__((section(".iobuf")))` for the pico1calc IOBUF
   region. xtensa_cc has no PPAP linker script (it inherits ESP-IDF's),
   so the attribute either has to drop on xtensa_cc or be guarded.
   Cleanest fix: replace the literal attribute with a
   `PPAP_IOBUF_SECTION` macro defined per-target (empty on xtensa_cc,
   `__attribute__((section(".iobuf")))` on pico1calc). Buffers fall
   into default DRAM on xtensa_cc — fine, since DRAM is plentiful and
   the totals are <1 KB.

2. **ESP-IDF `spi_master` component must be reachable.** It's normally
   pulled in automatically when a component depends on `driver` /
   `esp_driver_spi_master`. Add it to the `REQUIRES` list of
   `esp_idf/components/ppap_kernel/CMakeLists.txt` so CC-4b's first-cut
   transport links. Skip if CC-4b commits straight to bare MMIO at
   SPI2 base (same shape as the existing `usb_serial_jtag_ll_*`
   pattern in `usj.c`).

3. **Font-generation pipeline must live in the ESP-IDF component
   CMakeLists, not the project shell.** pico1calc invokes `bdf2c.py` /
   `json2c.py` from its top-level `CMakeLists.txt`. xtensa_cc's
   top-level is the ESP-IDF project shell that delegates everything
   to `esp_idf/components/ppap_kernel/`; the `add_custom_command` +
   `target_sources` for the generated font has to go there so the
   ESP-IDF build sees it.

**Substeps:**

| Step | Description |
|------|-------------|
| CC-4a | Refactor for multi-panel support: (1) rename `src/kernel/vfs/driver/lcd_panel.c` → `lcd_st7365p.c` and update pico1calc's CMakeLists to match (no behavioural change); (2) lift `LCD_WIDTH`/`LCD_HEIGHT` out of `lcd_panel.h` into a per-target geometry header at `target/<t>/kernel/vfs/driver/lcd_geom.h` — VFS subtree because LCD geometry is VFS-only (no core code references it). PicoCalc keeps 320×320; xtensa_cc gets 240×135. (3) Replace the literal `__attribute__((section(".iobuf")))` on `fbcon.c`'s `cell_char`/`cell_attr` with a `PPAP_IOBUF_SECTION` macro defined locally at the top of `fbcon.c` (one-file consumer, no header needed); xtensa_cc overrides it to empty via `-DPPAP_IOBUF_SECTION=` in its existing `target_compile_definitions`. PicoCalc keeps IOBUF placement, xtensa_cc lands the buffers in default DRAM. |
| CC-4b | Implement SPI2 transport in `src/target/xtensa_cc/kernel/vfs/driver/spi_lcd_xtensa_cc.c`, exporting the existing `spi_lcd.h` API. Phase 1 uses ESP-IDF `spi_master` (queued transactions, DMA-capable buffers); MOSI=35, SCK=36, CS=37, DC=34, RST=33 from `xtensa_cc.h`. Validate with a known-pattern fill before wiring fbcon. |
| CC-4c | Add `src/kernel/vfs/driver/lcd_st7789.c` (sibling to `lcd_panel.c`) implementing `lcd_init()` and `lcd_fill_rect()` for ST7789V2: hard reset → SLPOUT (120 ms) → COLMOD=0x55 → MADCTL (landscape rotation) → CASET/RASET for the 240×135 visible window with the controller's 40-px column offset — this is the usual ST7789V2 gotcha — → INVON (IPS panels need inversion) → DISPON; then PWM the backlight on GPIO38. Both targets pick exactly one panel driver via CMakeLists. |
| CC-4d | Add 8×8 font generation to `src/target/xtensa_cc/CMakeLists.txt` (mirror pico1calc's `bdf2c.py` / `json2c.py` invocation for `font8x8.c`); link `fbcon.c`, `lcd_st7789.c`, the new SPI transport, and the generated font into the ESP-IDF component. |
| CC-4e | In `xtensa_cc_logger.c`: extend `vfs_notify()` with `VFS_EVENT_LATE_INIT` → `spi_lcd_init() → lcd_init() → fbcon_init() → klog_set_logger(KLOG_LOGGER_SECONDARY, fbcon_putc, fbcon_flush) → tty_set_backend(TTY_DISPLAY, &fbcon_backend)`; and `VFS_EVENT_IDLE` → `fbcon_poll_flush()`. (The `getc`/`rx_avail` slots in the backend can stay NULL until CC-5 lands the keyboard.) |
| CC-4f | Update `target_caps()` to include `TARGET_CAP_SPI | TARGET_CAP_DISPLAY`; verify boot banner mirrors to LCD. |

**Open questions / risks specific to CC-4:**

- ST7789V2 column offset (typically 40 px in landscape, but depends on
  MADCTL rotation chosen) — verify on hardware with a 1-px border fill
  before declaring CC-4c done.
- ESP-IDF `spi_master` allocates DMA buffers from internal heap; this
  conflicts with CC-3.5b ("replace ad-hoc ESP-IDF heap usage with PPAP
  region allocators"). Acceptable as a CC-4 bring-up shortcut, but call
  it out as the natural trigger for finishing CC-3.5e (lower to bare
  MMIO + PPAP-owned DMA descriptors).
- Deferred-flush concurrency in `fbcon_flush_deferred` / `fbcon_poll_flush`
  was designed against the dual-core RP2040 model; on the single-core
  CC build the existing guard is sufficient, but the `volatile`
  ordering still matters once CC-3.5c (preemptive interrupt-return
  switching) lands.

### Phase CC-5: Keyboard

| Step | Description |
|------|-------------|
| CC-5a | GPIO matrix scan, debounce, ASCII keymap |
| CC-5b | Wire to `tty1` input ring buffer |
| CC-5c | Interactive shell on the CardComputer display |

### Phase CC-6: SD Card

| Step | Description |
|------|-------------|
| CC-6a | HSPI driver for microSD |
| CC-6b | FAT32 read support (or reuse existing SD driver) |
| CC-6c | Mount SD as `/mnt/sd`, set `TARGET_CAP_SD` |

---

## 8. Risks and Open Questions

| Risk | Mitigation |
|------|-----------|
| 512 KB SRAM is tight with 63 KB framebuffer | Text-mode-only buffer (~2 KB) with on-demand glyph rendering |
| SPI display + keyboard + SD share the SPI bus | Use separate SPI hosts (SPI2 for display, SPI3/HSPI for SD) |
| Keyboard matrix pin assignments not fully documented | Reverse-engineer from M5Stack Arduino library source |
| Display refresh rate with text-mode rendering | Dirty-rect flushing keeps SPI transfers small; 80 MHz SPI is fast enough |

---

## 9. References

- [M5Stack CardComputer product page](https://docs.m5stack.com/en/core/CardComputer)
- [M5Stack CardComputer schematic](https://docs.m5stack.com/en/core/CardComputer) (pinout section)
- [ST7789V2 datasheet](https://www.newhavendisplay.com/appnotes/datasheets/LCDs/ST7789V2.pdf)
- [ESP-IDF SPI Master Driver](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/spi_master.html)
- [Xtensa architecture details](../targets/xtensa.md)
