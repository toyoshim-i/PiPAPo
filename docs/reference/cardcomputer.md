# M5Stack CardComputer Hardware Specifications

This document summarizes the hardware specifications and development
information for the M5Stack CardComputer (also written
"M5Cardputer" — same device).  This file holds only the device-facing
facts; PPAP-side design choices for the shipped port live in
[`docs/targets/xtensa.md` §8](../targets/xtensa.md#8-cardcomputer-xtensa_cc-target-choices),
and remaining open work is split across
[`xtensa_xip.md`](../proposals/xtensa_xip.md) (user-space XIP),
[`sd_card.md`](../proposals/sd_card.md) (microSD), and
[`xtensa_baremetal_port.md`](../proposals/xtensa_baremetal_port.md)
(future bare-metal target).

## Hardware Overview

- **Core Module**: STAMP S3 — ESP32-S3-FN8 (Xtensa LX7 dual-core, 240 MHz)
- **Flash**: 8 MB QIO
- **SRAM**: 512 KB on-chip; **no PSRAM** on the STAMP S3 module
- **Wireless**: Wi-Fi 4 (802.11 b/g/n) + Bluetooth 5.0 / BLE
- **Display**: 1.14-inch IPS LCD, 240×135 px, 65K colors
  - Controller: Sitronix ST7789V2
  - Interface: SPI2 (TX-only, no MISO line)
  - Backlight: GPIO-controlled (no dedicated PWM IC)
- **Keyboard**: 56-key membrane keyboard (QWERTY)
  - Scan: 3-bit counter + external 1-of-8 demultiplexer + 7 input pins
  - Logical layout: 4 rows × 14 columns
- **Storage**: microSD slot (SPI / HSPI)
- **Audio**: NS4168 I²S amplifier + small speaker
- **IR**: 38 kHz IR transmitter (carrier shared with UART0 TX/I²S BCLK)
- **USB**: USB-C → ESP32-S3 native USB (USB Serial JTAG + USB OTG)
- **Expansion**: Grove port (I²C)
- **Power**: USB-C, internal LiPo battery

```
+------------------------------------------------+
|  M5Stack CardComputer                          |
|                                                |
|  +------------------+   +------------------+   |
|  | STAMP S3 Module  |   | ST7789V2 Display |   |
|  | (ESP32-S3-FN8)   |-->| 240x135 IPS      |   |
|  | 8 MB Flash (QIO) |   | SPI interface    |   |
|  | 512 KB SRAM      |   +------------------+   |
|  | Wi-Fi + BLE 5.0  |                          |
|  +------------------+   +------------------+   |
|         |               | 56-key Keyboard  |   |
|         +-------------->| 3-bit cnt + demux|   |
|         |               +------------------+   |
|         |                                      |
|         +---> microSD slot (SPI)               |
|         +---> I2S speaker (NS4168)             |
|         +---> IR transmitter (GPIO44)          |
|         +---> USB-C (native USB)               |
|         +---> Grove port (I2C)                 |
+------------------------------------------------+
```

## Peripheral Pin Assignments

| Peripheral | Interface | Pins | Notes |
|-----------|-----------|------|-------|
| ST7789V2 display | SPI2 | MOSI=35, SCK=36, CS=37, DC=34, RST=33, BL=38 | TX-only, 240×135 RGB565 |
| Keyboard counter | GPIO output | 8, 9, 11 (LSB→MSB) | Drives 3-bit value to external 1-of-8 demux |
| Keyboard input | GPIO INPUT_PULLUP | 13, 15, 3, 4, 5, 6, 7 | Active low |
| microSD | SPI3 / HSPI | MISO=39, MOSI=14, SCK=40, CS=12 | FAT32 |
| Speaker | I²S | BCLK=41, LRCK=43, DIN=42 | NS4168 amplifier |
| IR TX | GPIO | GPIO44 | 38 kHz modulation |
| USB | Native USB | GPIO19 (D−), GPIO20 (D+) | USB Serial JTAG + USB OTG |
| Grove I²C | I²C | SDA=2, SCL=1 | External expansion |
| UART0 | UART | TX=43, RX=44 | **Pin-shared with I²S LRCK and IR TX — unusable as console** |

## Display: ST7789V2

- **Controller**: Sitronix ST7789V2 — 240×320 RAM frame
- **Visible window**: 240×135 in landscape, sitting at column offset
  40, row offset 53 in the controller frame (when MADCTL = MV|MX)
- **Pixel format**: 16-bit RGB565 (MIPI DCS `COLMOD = 0x55`)
- **Recommended SPI clock**: ≤ 62.5 MHz per ST7789V2 spec; PPAP uses
  40 MHz on first cut
- **Frame rate**: hardware can refresh ~60 Hz; PPAP polls at the
  display flush cadence (~20 ms ≈ 50 Hz)

### Init Sequence (MIPI DCS)

The standard ST7789V2 init sequence used by every reference driver:

1. Hardware reset: assert RST low for ≥ 10 ms, release, wait ≥ 120 ms
2. `SLPOUT` (0x11), wait ≥ 120 ms
3. `COLMOD` (0x3A) = 0x55 (16-bit RGB565)
4. `MADCTL` (0x36) = chosen orientation byte
5. `INVON` (0x21) — IPS panels need inversion ON
6. `NORON` (0x13) — leave partial / idle modes
7. `CASET` / `RASET` / `RAMWR` (0x2A / 0x2B / 0x2C) — set window in
   *controller frame* coordinates (visible coords + offsets)
8. `DISPON` (0x29), wait ≥ 120 ms
9. Backlight pin high (GPIO38)

### MADCTL / Offsets per Orientation

| Orientation | MADCTL | Visible | Column offset | Row offset |
|-------------|--------|---------|---------------|------------|
| Landscape, USB-C right | 0x60 (MV \| MX) | 240×135 | 40 | 53 |
| Landscape, USB-C left  | 0xA0 (MV \| MY) | 240×135 | 40 | 53 |
| Portrait               | 0x00            | 135×240 | 52 | 40 |

The M5Cardputer's stock orientation is **landscape with USB-C on the
right** (keyboard at the bottom, screen above).

## Keyboard

### Matrix Topology

The 56-key keyboard is **not** a direct GPIO matrix.  Per the M5Stack
M5Cardputer Arduino library (`src/utility/Keyboard/KeyboardReader/IOMatrix.{h,cpp}`):

- **3 output pins** (GPIO 8, 9, 11) form the low 3 bits of an 8-state
  counter.  External logic on the PCB (74HC138 or equivalent discrete
  logic) feeds a 1-of-8 demultiplexer that grounds exactly one of 8
  column groups per counter step.
- **7 input pins** (GPIO 13, 15, 3, 4, 5, 6, 7) configured as
  `INPUT_PULLUP`, idle high.  A pressed key in the active column
  pulls one or more inputs to GND.
- 8 counter values × 7 input pins = **56 keys total**.
- Decoded into a logical **4-row × 14-column** keymap.

### Scan Algorithm

```
for cnt in 0..7:
    drive (gpio8, gpio9, gpio11) = (cnt&1, (cnt>>1)&1, (cnt>>2)&1)
    settle briefly  (~µs; ESP32-S3 GPIO is fast)
    read input bitmap = 7-bit mask of (gpio13,15,3,4,5,6,7) inverted
    for each set bit, decode (logical_x, logical_y)
```

Decode mapping from `(counter, input_bit)` to `(logical_x,
logical_y)` uses the `X_map_chart` lookup table in
`KeyboardReader/IOMatrix.h:32`:

```
X_map_chart[7] = {{1,  0,  1},  {2,  2,  3},  {4,  4,  5},
                  {8,  6,  7},  {16, 8,  9},  {32, 10, 11}, {64, 12, 13}}
```

- Counter values 0..3 → `logical_x = X_map_chart[input].x_2`
- Counter values 4..7 → `logical_x = X_map_chart[input].x_1`
- Logical y = `(counter > 3) ? counter - 4 : counter`, then flipped
  so y=0 is the top (number) row.

The upstream library does **no software debounce** — relies on the
natural poll cadence (~30–60 Hz) being slow relative to ESP32-S3
GPIO sampling but fast relative to typical 1–5 ms key bounce.

### Keymap (base + Shift)

Transcribed from `Keyboard.h:18-73` (`_key_value_map[4][14]`).
Each cell is `{value_first, value_second}`; `value_second` is used
when Shift, Ctrl, or CapsLock is active.

**Row y=0** (number row, 14 keys):

| x  | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 |
|----|---|---|---|---|---|---|---|---|---|---|----|----|----|----|
|base| `` ` `` | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 0 | `-` | `=` | BkSp |
|shft| `~` | `!` | `@` | `#` | `$` | `%` | `^` | `&` | `*` | `(` | `)` | `_` | `+` | BkSp |

**Row y=1** (QWERTY top, 14 keys):

| x  | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 |
|----|---|---|---|---|---|---|---|---|---|---|----|----|----|----|
|base|TAB| q | w | e | r | t | y | u | i | o | p | `[`| `]`| `\`|
|shft|TAB| Q | W | E | R | T | Y | U | I | O | P | `{`| `}`| `\|`|

**Row y=2** (home row, 14 keys):

| x  | 0  | 1   | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 |
|----|----|-----|---|---|---|---|---|---|---|---|----|----|----|----|
|base| Fn |Shift| a | s | d | f | g | h | j | k | l | `;`| `'`|ENT |
|shft| Fn |Shift| A | S | D | F | G | H | J | K | L | `:`| `"`|ENT |

**Row y=3** (bottom, 14 keys):

| x  | 0  | 1  | 2  | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 |
|----|----|----|----|---|---|---|---|---|---|---|----|----|----|----|
|base|Ctrl|Opt |Alt | z | x | c | v | b | n | m | `,`| `.`| `/`|SPC |
|shft|Ctrl|Opt |Alt | Z | X | C | V | B | N | M | `<`| `>`| `?`|SPC |

### Modifier Keys

| Key | Coord (y, x) | Hardware role |
|-----|--------------|---------------|
| Shift | (2, 1) | Selects the `shift` row of the keymap. |
| Fn    | (2, 0) | State-only flag in the upstream library — no Fn-layer keymap is defined; applications choose. |
| Ctrl  | (3, 0) | State flag; upstream library also routes the keystroke through `value_second` (likely a quirk for HID forwarding). |
| Opt   | (3, 1) | State-only flag; reserved for application use. |
| Alt   | (3, 2) | State-only flag. |

### Special Keys

- **Tab**: y=1, x=0
- **Enter**: y=2, x=13
- **Backspace**: y=0, x=13
- **Space**: y=3, x=13
- **Esc**: **does not exist** on this keyboard.  Software must synthesize.

### Silkscreen Icons (Fn-combo glyphs)

The Cardputer's keys are silkscreened with Fn-combo glyphs.  These
are **conventions**, not hardware mappings — the keyboard reports
only `(Fn pressed, key pressed)`; software must implement the layer.
The icons are:

- Fn+`;` → ↑
- Fn+`,` → ←
- Fn+`.` → ↓
- Fn+`/` → →
- Fn+`` ` `` → Esc
- Fn+1..0 → F1..F10
- Fn+Backspace → Delete

## microSD

| Parameter | Value |
|-----------|-------|
| SPI host | HSPI (SPI3_HOST) |
| MISO | GPIO39 |
| MOSI | GPIO14 |
| SCK | GPIO40 |
| CS | GPIO12 |

Standard SPI-mode SD card protocol; FAT32 filesystem typical.

## USB Serial JTAG

The ESP32-S3 native USB peripheral on GPIO19 (D−) / GPIO20 (D+)
exposes a USB Serial JTAG (USJ) device when not configured as USB
OTG.  Appears as `/dev/ttyACM*` on the host.  Used for both flashing
(via `esptool.py`) and serial console.

USJ is the **only practical console** on this board because UART0's
TX/RX (GPIO43/44) are physically wired to I²S LRCK and the IR TX —
driving them as UART would conflict with the audio amp and IR
emitter.

## Memory Map (ESP32-S3)

- **IRAM**: `0x40370000`–`0x403DFFFF` (instruction bus, 32-bit
  word-access only)
- **DRAM**: `0x3FC88000`–`0x3FCFFFFF` (data bus, byte-accessible)
- **Flash XIP**: `0x42000000` (instruction-cached) /
  `0x3C000000` (data-cached)
- **Peripherals**: `0x60000000`+ (e.g. GPSPI2 at 0x60024000)
- **No PSRAM** on the STAMP S3 module — `CONFIG_SPIRAM_IGNORE_NOTFOUND=y`
  makes ESP-IDF accept this gracefully.

See [`docs/targets/xtensa.md`](../targets/xtensa.md) for the ISA
and toolchain details.

## References

- [M5Stack CardComputer product page](https://docs.m5stack.com/en/core/CardComputer)
- [M5Stack CardComputer schematic](https://docs.m5stack.com/en/core/CardComputer) (pinout section)
- [m5stack/M5Cardputer (Arduino library)](https://github.com/m5stack/M5Cardputer) — keymap source of truth; relevant files: `src/utility/Keyboard/Keyboard.h`, `Keyboard.cpp`, `Keyboard_def.h`, `KeyboardReader/IOMatrix.{h,cpp}`
- [ST7789V2 datasheet](https://www.newhavendisplay.com/appnotes/datasheets/LCDs/ST7789V2.pdf)
- [ESP32-S3 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf)
- [Xtensa architecture details](../targets/xtensa.md)
