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
| USB | Native USB | GPIO19 (D-), GPIO20 (D+) | CDC-ACM for UART |
| UART0 | UART | TX=43, RX=44 | Shared with I2S/IR |

### 1.3 Console Strategy

- **Primary console (`ttyS0`)**: USB CDC-ACM via ESP32-S3's native USB
  (appears as `/dev/ttyACM0` on host). Used for development and flashing.
- **Display console (`tty1`)**: ST7789 + keyboard — standalone terminal.
- Default console: `tty1` when keyboard is detected (always present on
  CardComputer).

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

Phases CC-1 through CC-3 (architecture bring-up, interrupts, context
switch, syscalls, user-space binaries) are complete or in progress.
See [xtensa.md](../targets/xtensa.md) §8 for current status.

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

| Step | Description |
|------|-------------|
| CC-3.5a | Define explicit PPAP-owned memory regions for IRAM text, DRAM user data, kernel DRAM, and device/DMA use |
| CC-3.5b | Replace ad-hoc ESP-IDF heap usage in the Xtensa loader with PPAP region allocators |
| CC-3.5c | Move exception / interrupt ownership as fully as possible under PPAP after `app_main()` |
| CC-3.5d | Reintroduce PMS with a PPAP-defined user/kernel memory map |
| CC-3.5e | Prefer direct MMIO drivers for GPIO/SPI/I2C/UART once the bootstrap phase is complete |

### Phase CC-4: Display

| Step | Description |
|------|-------------|
| CC-4a | SPI2 driver for ST7789V2 (init sequence, pixel write) |
| CC-4b | Framebuffer console: 8x8 font, 30x16 text grid |
| CC-4c | Register `tty_backend_t` for `tty1`, hook display poll |
| CC-4d | Boot messages visible on LCD |

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
