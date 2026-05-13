# M5Stack CardComputer Target Support

Device-specific PPAP porting plan for the M5Stack CardComputer
(`xtensa_cc`) target.  Pure hardware facts — block diagram, pin
assignments, ST7789V2 datasheet info, keyboard matrix topology and
keymap, microSD pinout, memory map — live in
[`docs/reference/cardcomputer.md`](../reference/cardcomputer.md).
For Xtensa architecture details (ISA, toolchains, memory model, trap
handling, ELF loading), see [`docs/targets/xtensa.md`](../targets/xtensa.md).

This file holds: PPAP-side design choices (driver placement, init
order, modifier-key behavior we picked), the implementation plan
(phases CC-1 .. CC-6 with substeps), and the open questions specific
to the port.

## 1. Console Strategy (PPAP design)

- **Primary console (`ttyS0`)**: USB Serial JTAG via ESP32-S3's
  native USB.  USJ is the only practical console because UART0's
  TX/RX (GPIO43/44) are physically shared with I²S LRCK and the IR
  TX line — see
  [reference §Peripheral Pin Assignments](../reference/cardcomputer.md#peripheral-pin-assignments).
- **Display console (`tty1`)**: ST7789V2 + keyboard.  Joins as
  `KLOG_LOGGER_SECONDARY` on top of the USJ primary, matching the
  pico1calc UART+FBCON pattern.
- Default console: `ttyS0` (USJ).  `tty1` is the auto-login primary
  in `/etc/inittab` (per pico1calc convention) once CC-5 makes it
  interactive.

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

## 4. Display Driver Design

Hardware reference: [reference §Display: ST7789V2](../reference/cardcomputer.md#display-st7789v2).

PPAP-side design choices for the display driver:

- **Strategy**: text-mode cell buffer + scanline-streaming SPI flush
  (no full 63 KB RGB565 framebuffer in SRAM).  Reuse pico1calc's
  fbcon pipeline verbatim — ~1920 B for 60×16 cell+attr, plus a
  480-byte stack scanline buffer during flush.
- **Driver placement**:
  - Generic ST7789V2 controller init at
    `src/kernel/vfs/driver/lcd_st7789.c` (sibling to `lcd_st7365p.c`).
  - Target-specific SPI2 transport at
    `src/target/xtensa_cc/kernel/vfs/driver/spi_lcd_xtensa_cc.c`.
  - Per-target geometry / panel-frame offsets at
    `src/target/xtensa_cc/kernel/vfs/driver/lcd_geom.h`.
- **Orientation**: landscape with USB-C on the right (MADCTL =
  MV|MX, panel-frame offsets 40 col / 53 row).
- **Backlight**: plain GPIO output, asserted by target glue
  (`xtensa_cc_logger.c`) after `lcd_init()` returns so the
  post-reset garbage is not visible to the user.  PWM via `ledc`
  is a future option if brightness control is needed.
- **TTY grid**: 4×8 font → 60×16 character terminal
  (240 / 4 = 60, 128 / 8 = 16, with 7 px vertical slack).  MODE_SQUARE
  (8×8 → 30×16) is reachable via `fbcon_set_mode()` for callers that
  want bigger glyphs at the cost of half the column count.

Detailed substep breakdown is in §7's Phase CC-4.

---

## 5. Keyboard Driver Design

Hardware reference: [reference §Keyboard](../reference/cardcomputer.md#keyboard).
The keyboard is **not** a direct GPIO matrix — it uses a 3-bit
counter feeding an external 1-of-8 demux plus 7 INPUT_PULLUP lines
(10 GPIOs) decoded into a 4×14 logical keymap.  Pin sets, scan
algorithm, `X_map_chart` decode, full base+Shift keymap, modifier
key positions, and the silkscreen icons all live in the reference
doc.

PPAP-side design choices for the keyboard driver:

- **Driver placement**: target-specific
  `src/target/xtensa_cc/kernel/vfs/driver/cc_kbd.c` (matrix scan +
  decode is bound to ESP32-S3 GPIO and the M5Cardputer-specific
  topology — no other target shares it).  Keymap data in
  `cc_keymap.h` next to the driver.
- **Driver API**: same minimal contract as pico1calc's `i2c_kbd.h`:
  ```c
  void kbd_init(void);
  int  kbd_poll(void);       /* one byte, or -1 if none */
  int  kbd_poll_avail(void); /* non-zero if a byte is available */
  ```
- **Polling**: at the existing display flush cadence (~20 ms via
  `vfs_notify(VFS_EVENT_IDLE)`).  Edge-only delivery via a
  previous-poll bitmap snapshot.  Upstream has no debounce; the
  20 ms cadence is well above typical 1–5 ms key bounce.
- **Ctrl-C hoist**: bytes equal to 0x03 are routed to
  `tty_signal_intr(TTY_DISPLAY)` before being queued, so a
  compute-bound foreground task does not need to be blocked in
  `read()` to receive SIGINT.  Mirrors pico1calc.
- **Multi-byte escapes**: arrow keys, F-keys, Esc, Delete all emit
  VT100 escape sequences.  `cc_kbd.c` stores them as null-terminated
  strings and `kbd_poll()` returns one byte at a time via an
  internal sequence cursor (mirrors `i2c_kbd.c`).

### 5.1 PPAP Fn-Layer (matches the silkscreen)

The upstream M5Cardputer library tracks Fn as a state flag and
defines no Fn-layer keymap; applications choose.  PPAP's choice
matches the icons silkscreened on the keys (see [reference
§Silkscreen Icons](../reference/cardcomputer.md#silkscreen-icons-fn-combo-glyphs)):

| Combo | Output | Notes |
|-------|--------|-------|
| Fn+`;` | `\033[A` (↑) | Arrow up — silkscreen places `;` directly above `,` `.` `/` |
| Fn+`,` | `\033[D` (←) | Arrow left |
| Fn+`.` | `\033[B` (↓) | Arrow down |
| Fn+`/` | `\033[C` (→) | Arrow right |
| Fn+`` ` `` | `\033` (Esc) | Cardputer has no dedicated Esc key |
| Fn+1..0 | `\033OP`..`\033OY` (F1–F10) | Standard VT100 PF / F-key codes |
| Fn+Backspace | `\033[3~` (Delete) | Forward-delete |

Other modifiers:

| Key | PPAP behavior |
|-----|---------------|
| Shift | Selects the `shift` row of the keymap for the next non-modifier key. |
| Ctrl  | Combines with letters → control codes (Ctrl-A=0x01 .. Ctrl-Z=0x1A).  Ctrl-C is hoisted to `tty_signal_intr(TTY_DISPLAY)` before being delivered. |
| Opt   | Tracked only; reserved for application use.  No character transformation in CC-5. |
| Alt   | Tracked only; reserved.  Future M-prefix VT escape support is possible but not in CC-5 scope. |

### 5.2 Integration with `xtensa_cc_logger.c`

CC-4 left `fbcon_backend.getc` / `.rx_avail` NULL.  CC-5 fills them
by mirroring pico1calc's pattern:

1. Add a 16-byte ring buffer in `xtensa_cc_logger.c`
   (head/tail volatile, power-of-two for mask wrap).
2. Add `fbcon_getc_wrapper` / `fbcon_avail_wrapper` that consume
   from the ring buffer; `_avail_wrapper` drains up to 8 events from
   `kbd_poll()` per call (bounded loop) and feeds the ring.
3. Hoist Ctrl-C as above.
4. Fill the NULL backend slots and call `kbd_init()` from
   `vfs_notify(VFS_EVENT_LATE_INIT)` after the existing
   `tty_set_backend(TTY_DISPLAY, &fbcon_backend)`.

The pin-defs prerequisite (the broken `KBD_ROW_PINS`/`KBD_COL_PINS`
in `xtensa_cc.h`) is captured in §7's CC-5a.

---

## 6. microSD Card Design

Hardware reference: [reference §microSD](../reference/cardcomputer.md#microsd).
HSPI (SPI3_HOST), MISO=39, MOSI=14, SCK=40, CS=12.

PPAP-side plan:

- Reuse PPAP's existing `spi_sd.c` driver.  An xtensa_cc-specific
  HSPI transport wrapper similar to `spi_lcd_xtensa_cc.c` will host
  the ESP-IDF `spi_master` glue for SPI3.
- FAT32 read support via the existing `vfat.c` mount path.
- Mount as `/mnt/sd`, set `TARGET_CAP_SD` in `target_caps()`.

Detailed substep breakdown is in §7's Phase CC-6.

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
| CC-4: ST7789 display + framebuffer console | DONE |
| CC-5: keyboard scan + `tty1` input (3-bit-counter matrix) | DONE |
| CC-6: microSD over HSPI | not started |

User-space boots to working `push` shell prompts on both USJ and the
LCD: USJ is the secondary getty (`getty ttyS0`, wait-for-Enter) and
the LCD is the auto-login primary (`getty -l tty1`).  Keystrokes are
delivered on both paths — USJ via tty_poll_input on the USJ RX FIFO,
LCD via the GPIO-matrix scanner draining into the fbcon backend's
ring buffer.  exec / vfork / signal-action paths are exercised on
both consoles, including Ctrl-C interrupt delivery via
`tty_signal_intr(TTY_DISPLAY)`.  `target_caps()` advertises
`TARGET_CAP_SPI | TARGET_CAP_DISPLAY | TARGET_CAP_KBD`.

### Known Gaps (tracked, not phase-blocking)

- **Keyboard pin defs in `xtensa_cc.h` are wrong** (CC-5a fixes
  this).  The current `KBD_ROW_PINS` / `KBD_COL_PINS` describe a
  fictional 7×8 direct-GPIO matrix; the real hardware is a 3-bit
  counter feeding an external 1-of-8 demux plus 7 INPUT_PULLUP
  lines.  No code currently consumes the broken defines, so this
  is dormant until CC-5 starts.  See [reference §Keyboard](../reference/cardcomputer.md#keyboard) for the correct pin layout.

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

CC-4 already left `fbcon_backend.getc` / `fbcon_backend.rx_avail`
NULL; CC-5 fills those in so the LCD shell prompt becomes
interactive.  Hardware (matrix topology, scan algorithm, keymap,
silkscreen icons) is in [reference §Keyboard](../reference/cardcomputer.md#keyboard);
PPAP design (driver placement, Fn-layer mapping, integration with
fbcon backend) is in [§5](#5-keyboard-driver-design) above.

**Reuse map (pico1calc → xtensa_cc):**

| Layer | pico1calc artifact | xtensa_cc plan |
|-------|--------------------|----------------|
| Ring buffer + drain wrapper + Ctrl-C hoist | `pico1calc_logger.c:25-73` | mirror in `xtensa_cc_logger.c` |
| Backend slot wiring | `fbcon_backend.{getc,rx_avail}` set in `pico1calc_logger.c:127-128` | fill the NULL slots in `xtensa_cc_logger.c::fbcon_backend` |
| Bounded poll loop | "drain up to 8 key events per poll cycle" | reuse the same 8-events bound |
| Multi-byte escape buffering | `i2c_kbd.c` returns one byte per `kbd_poll()` call (escape-sequence FIFO inside the driver) | same pattern in `cc_kbd.c` for arrow / F-key escapes |
| Driver itself | `i2c_kbd.c` (I2C protocol, generic) | new `target/xtensa_cc/kernel/vfs/driver/cc_kbd.c` (3-bit-counter matrix scan, target-specific) |

**Prerequisites (must land before any scan code is written):**

1. **Fix the keyboard pin defs in `xtensa_cc.h`.**  Replace the
   incorrect `KBD_ROW_PINS` / `KBD_COL_PINS` (which conflate output
   drives with input lines and re-use GPIOs 5/6/7 in both lists)
   with the actual hardware pin sets:
   ```c
   #define KBD_OUT_PINS  { 8, 9, 11 }            /* 3-bit counter LSB->MSB */
   #define KBD_IN_PINS   { 13, 15, 3, 4, 5, 6, 7 } /* INPUT_PULLUP, active low */
   #define KBD_NUM_OUT   3
   #define KBD_NUM_IN    7
   ```
   Old `KBD_NUM_ROWS` / `KBD_NUM_COLS` go away.  No other code
   currently references the old defines, so the rename is mechanical.

2. **No new arch primitives required.**  Unlike CC-4b's
   `spi_master`, the matrix scan only needs `gpio_set_direction`,
   `gpio_set_level`, and `gpio_get_level` — all already pulled in
   via the existing `driver` ESP-IDF component dependency.

**Substeps:**

| Step | Description |
|------|-------------|
| CC-5a | Pin-fix prerequisite: rewrite the keyboard section of `xtensa_cc.h` (`KBD_OUT_PINS` + `KBD_IN_PINS`).  No-op build-wise until CC-5b consumes them. |
| CC-5b | Implement `target/xtensa_cc/kernel/vfs/driver/cc_kbd.c`: configure 3 OUTPUT + 7 INPUT_PULLUP pins; `kbd_poll()` runs the 8-step counter sweep, decodes `(counter, input)` to `(logical_x, logical_y)` via the `X_map_chart` lookup, and looks up the keymap.  Edge-only delivery via a previous-poll bitmap snapshot. |
| CC-5c | Add the keymap header `target/xtensa_cc/kernel/vfs/driver/cc_keymap.h`: the 4×14 base + shift table (transcribed from [reference §Keymap](../reference/cardcomputer.md#keymap-base--shift)), plus the PPAP Fn-layer table (silkscreen mapping per [§5.1](#51-ppap-fn-layer-matches-the-silkscreen)).  Multi-byte escapes (arrows, F-keys) are stored as null-terminated strings; `kbd_poll()` returns them one byte at a time via an internal sequence cursor. |
| CC-5d | Add the kbd ring buffer + drain wrappers in `xtensa_cc_logger.c` (mirror `pico1calc_logger.c:25-73`).  Fill in `fbcon_backend.getc` / `.rx_avail`.  Hoist Ctrl-C via `tty_signal_intr(TTY_DISPLAY)`.  Call `kbd_init()` from `vfs_notify(VFS_EVENT_LATE_INIT)` after `tty_set_backend(TTY_DISPLAY, &fbcon_backend)`. |
| CC-5e | Update `target_caps()` in `target_xtensa_cc.c` to include `TARGET_CAP_KBD`.  On hardware, verify: shell prompt accepts typed characters; arrow keys move within line editor; Ctrl-C interrupts a foreground task; Esc emits via Fn+`` ` ``. |

**Open questions / risks specific to CC-5:**

- **Polling cadence vs key-repeat.**  The fbcon idle-flush poll runs
  at ~20 ms.  Holding a key produces one event per scan cycle, so
  the natural repeat rate is ~50 Hz.  That's faster than typical
  shell autorepeat (~25 Hz) — may need a per-key timestamp to
  throttle.  Defer until visible on hardware.
- **Ghost keys.**  3-key combinations on the same column can
  produce a phantom press at the matrix intersection.  Upstream
  M5Cardputer doesn't filter for it; PPAP can adopt the same "first
  press wins" stance and revisit only if it bites.
- **Modifier-state race.**  Shift / Fn / Ctrl pressed and released
  between two scan cycles is observable; pressed and released
  *within* a single scan cycle is lost.  At 20 ms cycle this is a
  ~50 ms minimum hold time, which is below typical human latency
  but above competitive-typing teardown times.  Acceptable.
- **No Esc key.**  Ships as Fn+`` ` ``.  Apps that grew up assuming
  a dedicated Esc key (vi, nano, less) need to learn the combo.
  Documented in §5.1 (Fn-layer); no kernel-side mitigation needed.

### Phase CC-6: SD Card

| Step | Description |
|------|-------------|
| CC-6a | HSPI driver for microSD |
| CC-6b | FAT32 read support (or reuse existing SD driver) |
| CC-6c | Mount SD as `/mnt/sd`, set `TARGET_CAP_SD` |

---

## 8. Risks and Open Questions

Per-phase risks are captured under each Phase CC-X section in §7.
This table covers cross-cutting concerns:

| Risk | Mitigation |
|------|-----------|
| 512 KB SRAM is tight if anything ever wants a full RGB565 framebuffer (63 KB) | Text-mode cell buffer + scanline streaming keeps fbcon under 1 KB SRAM; never allocate the full framebuffer. |
| Display SPI (SPI2) and microSD SPI (SPI3) on different controllers | Already separate hosts per [reference §Peripheral Pin Assignments](../reference/cardcomputer.md#peripheral-pin-assignments); no bus contention possible. |
| Display refresh latency with text-mode rendering | Dirty-row flushing keeps each SPI transfer to ≤ 1 row; at 40 MHz SPI a worst-case 60×16 grid flush is ≈ 2 ms. |

---

## 9. References

- [Hardware reference: docs/reference/cardcomputer.md](../reference/cardcomputer.md)
  — block diagram, pinout, ST7789V2 datasheet info, full keymap,
  external upstream links (M5Cardputer Arduino library, ST7789V2
  datasheet, ESP32-S3 TRM).
- [Xtensa architecture details](../targets/xtensa.md)
- [ESP-IDF SPI Master Driver](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/spi_master.html)
  — used by CC-4b's first-cut SPI2 transport.
- [Context-switch cleanup proposal](context_switch_cleanup.md)
  Phase 4 — tracks the Xtensa scheduler-stability work referenced
  under Known Gaps.
