# PicoCalc LCD Display — Technical Reference

This document describes the PicoCalc's LCD display hardware and the PPAP
driver stack that controls it. It serves as a reference for understanding
the initialization sequence, SPI transport, and framebuffer console layers.

## Hardware Summary

| Property          | Value                                          |
|-------------------|------------------------------------------------|
| Panel             | 4-inch 320x320 IPS LCD (RGB565, 65K colours)   |
| Controller IC     | ST7365P (ILI9488-compatible)                    |
| Internal RAM      | 320x480 (top 320 rows used)                    |
| Interface         | 4-wire SPI (TX-only, no MISO)                  |
| SPI peripheral    | RP2040 SPI1 (PL022) at 0x40040000              |
| SPI clock         | ~33 MHz (133 MHz / 4)                           |
| Pixel format      | RGB565 big-endian (16-bit, MSB first on SPI)    |
| Backlight control | I2C via STM32 (addr 0x1F, register 0x05, 0-255) |

## Pin Assignments

| Signal | GPIO | FUNCSEL | Notes                               |
|--------|------|---------|-------------------------------------|
| SCK    | GP10 | 1 (SPI) | SPI1 clock                          |
| MOSI   | GP11 | 1 (SPI) | SPI1 TX (data to LCD)               |
| CS     | GP13 | 5 (SIO) | Chip select, active low, manual GPIO|
| DC     | GP14 | 5 (SIO) | Data/Command select, manual GPIO    |
| RST    | GP15 | 5 (SIO) | Hardware reset, active low          |

CS, DC, and RST are controlled via the SIO block (direct GPIO) rather than
the SPI peripheral because the PL022 does not natively support the DC line
and CS must be held across multi-byte transfers.

## Driver Architecture

The driver is split into three layers:

```
+--------------------------------------------+
|  fbcon.c  —  Text console (VT100/ANSI)     |  40x20 or 80x40 chars
+--------------------------------------------+
|  lcd.c    —  Controller init + primitives   |  fill_rect
+--------------------------------------------+
|  spi_lcd.c — SPI1 transport (PL022)        |  cmd, data, window, fill
+--------------------------------------------+
|  Hardware: SPI1 + GPIO (CS/DC/RST)         |
+--------------------------------------------+
```

### Layer 1: SPI Transport (`spi_lcd.c`)

Drives the PL022 SPI1 controller. All LCD communication flows through
this layer.

**SPI Configuration:**

| Register | Value  | Meaning                                      |
|----------|--------|----------------------------------------------|
| CPSR     | 2      | Clock prescale divisor                       |
| CR0 SCR  | 1      | Serial clock rate (bits [15:8])              |
| CR0 DSS  | 0x07   | 8-bit data frames                            |
| CR0 FRF  | 0x00   | Motorola SPI frame format                    |
| CR0 CPOL | 0      | Clock idle low (SPI mode 0)                  |
| CR0 CPHA | 0      | Data sampled on rising edge                  |
| CR1 SSE  | 1      | SSP enable (master mode)                     |

Effective clock: 133 MHz / (CPSDVSR x (1 + SCR)) = 133 / (2 x 2) = 33.25 MHz.

**TX-only operation:** The LCD has no MISO line, but the PL022 still fills
its RX FIFO on every byte sent (the MISO pin floats). The driver drains the
RX FIFO every 4-8 bytes to prevent overflow (FIFO depth is 8 entries).

**Command vs Data:** The DC (Data/Command) pin distinguishes between
command bytes (DC low) and data/parameter bytes (DC high). The protocol is:

```
  cmd(0x2A)             →  DC=0, CS=0, send 0x2A, wait idle, CS=1
  data([x0h,x0l,...])   →  DC=1, CS=0, send bytes, wait idle, CS=1
```

CS is asserted (low) for the entire duration of each command or data
transfer and deasserted between them.

**Timeout protection:** Every SPI write polls the TX-not-full or BSY flags
with a ~50 ms timeout. On timeout, the `lcd_ok` flag is cleared and all
subsequent LCD operations become no-ops. This prevents the kernel from
hanging if the LCD hardware is absent or unresponsive.

### Layer 2: LCD Controller (`lcd.c`)

Sends the initialization command sequence and provides drawing primitives.

**Key facts about the ST7365P:**

- The controller is marketed as ILI9488-compatible but is actually an
  ST7365P (Sitronix). The standard MIPI DCS commands work, but RGB565
  over 4-wire SPI requires unlocking the vendor-specific extended command
  set first.
- The internal framebuffer is 320x480, but only the top 320 rows are
  visible on this panel. The address window is constrained accordingly.
- Display inversion (CMD 0x21) is required for correct colour rendering.

### Layer 3: Framebuffer Console (`fbcon.c`)

Character-cell text console rendered to the LCD via scanline streaming.

**Display modes:**

| Mode    | Columns | Rows | Font  | Char size |
|---------|---------|------|-------|-----------|
| Normal  | 40      | 20   | 8x16  | 8px wide, 16px tall |
| Compact | 80      | 40   | 4x8   | 4px wide, 8px tall  |

**Rendering strategy:** A full 320x320 RGB565 framebuffer would require
204,800 bytes — far too large for the RP2040's 264 KB SRAM. Instead, the
console maintains character and attribute cell arrays (6.4 KB total) and
renders one scanline at a time into a 640-byte stack buffer (320 pixels x
2 bytes). Only rows marked dirty are redrawn.

**Memory layout:**

| Buffer       | Size    | Section  | Purpose                       |
|--------------|---------|----------|-------------------------------|
| cell_char[]  | 3200 B  | .iobuf   | Character codes (ASCII)       |
| cell_attr[]  | 3200 B  | .iobuf   | Colour attributes (fg/bg)     |
| dirty[]      | 40 B    | .bss     | Per-row dirty flags           |
| line[] (stack)| 640 B  | stack    | Scanline render buffer        |

**Colour attributes:** Each cell has a 1-byte attribute: low nibble = foreground
colour (0-15), high nibble = background colour (0-15). The 16-colour ANSI
palette is mapped to RGB565:

| Index | Colour         | RGB565  |
|-------|----------------|---------|
| 0     | Black          | 0x0000  |
| 1     | Red            | 0x8000  |
| 2     | Green          | 0x0400  |
| 3     | Yellow         | 0x8400  |
| 4     | Blue           | 0x0010  |
| 5     | Magenta        | 0x8010  |
| 6     | Cyan           | 0x0410  |
| 7     | White (grey)   | 0xC618  |
| 8     | Bright black   | 0x8410  |
| 9     | Bright red     | 0xF800  |
| 10    | Bright green   | 0x07E0  |
| 11    | Bright yellow  | 0xFFE0  |
| 12    | Bright blue    | 0x001F  |
| 13    | Bright magenta | 0xF81F  |
| 14    | Bright cyan    | 0x07FF  |
| 15    | Bright white   | 0xFFFF  |

**VT100/ANSI escape sequences supported:**

| Sequence          | Name    | Description                        |
|-------------------|---------|------------------------------------|
| ESC [ n A         | CUU     | Cursor up n lines                  |
| ESC [ n B         | CUD     | Cursor down n lines                |
| ESC [ n C         | CUF     | Cursor forward n columns           |
| ESC [ n D         | CUB     | Cursor back n columns              |
| ESC [ r ; c H     | CUP     | Cursor position (row; col, 1-based)|
| ESC [ n J         | ED      | Erase display (0=below, 1=above, 2=all) |
| ESC [ n K         | EL      | Erase line (0=right, 1=left, 2=all)|
| ESC [ params m    | SGR     | Set graphic rendition (colours)    |
| ESC [ t ; b r     | DECSTBM | Set scroll region (top; bottom)    |
| ESC [ ? 80 h/l    | Private | Switch to 80-col / 40-col mode     |
| ESC c             | RIS     | Full terminal reset                |

SGR parameters: 0=reset, 1=bold, 22=normal, 30-37=fg, 39=default fg,
40-47=bg, 49=default bg, 90-97=bright fg, 100-107=bright bg.

## LCD Initialization Sequence

The complete init sequence as implemented in `lcd_init()`:

```
 Step  Command   Data                     Purpose
 ----  --------  -----------------------  -----------------------------------
  1    (GPIO)    RST low 10ms, high 120ms Hardware reset
  2    0x01      (none), wait 120ms       Software reset (SWRESET)
  3a   0xF0      0xC3                     Unlock ST7365P extended commands
  3b   0xF0      0x96                     Unlock ST7365P extended commands
  4    0x36      0x48 (MX|BGR)            Memory access: mirror X, BGR order
  5    0x3A      0x55                     Pixel format: RGB565 (16-bit)
  6    0xB1      0xA0                     Frame rate control
  7    0xB4      0x00                     Display inversion control
  8    0xB7      0xC6                     Entry mode set
  9a   0xC0      0x80, 0x06              Power control 1 (GVDD, AVDD)
  9b   0xC1      0x15                     Power control 2
  9c   0xC2      0xA7                     Power control 3
  9d   0xC5      0x04                     VCOM control 1
  10   0xE8      40 8A 00 00 29 19 AA 33  Vendor timing adjustment
  11   0xE0      F0 06 0F 05 04 20 37     Positive gamma (14 bytes)
                 33 4C 37 13 14 2B 31
  12   0xE1      F0 11 1B 11 0F 0A 37     Negative gamma (14 bytes)
                 43 4C 37 13 13 2C 32
  13a  0xF0      0x3C                     Re-lock extended commands
  13b  0xF0      0x69                     Re-lock extended commands
  14   0x21      (none)                   Display inversion on
  15   0x2A+0x2B 0,0 → 319,319           Set 320x320 address window
  16   0x11      (none), wait 120ms       Sleep out (SLPOUT)
  17   0x29      (none), wait 120ms       Display on (DISPON)
  18   (fill)    white pixels             Diagnostic fill (320x320)
```

**Why vendor unlock is needed:** The ST7365P defaults to 18-bit colour
(RGB666) over 4-wire SPI. To use the more bandwidth-efficient RGB565
format, the COLMOD register (0x3A) must be set to 0x55. On the ST7365P,
this value is only accepted when the extended command set is unlocked via
the 0xF0 command with keys 0xC3 and 0x96. Without unlocking, 0x3A=0x55
is silently ignored and the display shows corrupted colours.

**Why display inversion is on:** The panel's liquid crystal alignment
requires inversion for correct colour polarity. Without CMD 0x21, white
appears as black and colours are inverted.

**MADCTL (0x36) = 0x48:** MX (mirror X-axis) corrects for the panel's
physical mounting orientation on the PicoCalc board. BGR selects blue-green-red
subpixel order (the ST7365P's native subpixel layout).

## SPI Wire Protocol

A typical pixel write sequence on the SPI bus:

```
CS  ‾‾\_____/‾‾\_____________________________/‾‾\___/‾‾\_________/‾‾
DC  ‾‾\__low_/‾‾\___________high______________/‾‾\lo/‾‾\___high__/‾‾
MOSI    [0x2A]   [x0h] [x0l] [x1h] [x1l]        [2C]  [pxH][pxL]...
         CMD      Column address start/end        CMD    Pixel data
```

Each pixel is 2 bytes big-endian RGB565:
```
  Bit:  15 14 13 12 11 | 10  9  8  7  6  5 |  4  3  2  1  0
        R4 R3 R2 R1 R0 | G5 G4 G3 G2 G1 G0 | B4 B3 B2 B1 B0
        [  5-bit red  ] [   6-bit green    ] [  5-bit blue  ]
```

After CASET (0x2A) + RASET (0x2B) + RAMWR (0x2C), the controller auto-
increments its internal address pointer. Consecutive pixel data bytes fill
the window left-to-right, top-to-bottom without further commands.

## Backlight Control

The LCD backlight is controlled by the STM32 co-processor via I2C, not
by the RP2040 directly.

```
  I2C bus:       I2C1 (GP6 SDA, GP7 SCL) at 10 kHz
  Slave address: 0x1F
  Register:      0x05 (REG_ID_BKL)
  Range:         0 (off) to 255 (full brightness)
  Default:       128 (set during target init)
```

## Source Files

| File                                    | Purpose                            |
|-----------------------------------------|------------------------------------|
| `src/drivers/spi_lcd.c` / `.h`         | SPI1 transport layer               |
| `src/drivers/lcd.c` / `.h`             | Controller init + fill_rect        |
| `src/drivers/fbcon.c` / `.h`           | Text console + VT100 parser        |
| `src/drivers/font.h`                   | Font data (8x16 + 4x8 bitmaps)    |
| `src/target/pico1calc/pico1calc.h`     | Pin definitions                    |
| `src/target/pico1calc/target_pico1calc.c` | Board init (LCD + backlight)    |

## References

- Init sequence derived from [zenodante/PicoCalc-micropython-driver](https://github.com/zenodante/PicoCalc-micropython-driver) and the [official ClockworkPi PicoCalc repository](https://github.com/clockworkpi/PicoCalc)
- ST7365P is Sitronix silicon; datasheet not publicly available but register-compatible with ILI9488 for standard MIPI DCS commands
- MIPI DCS (Display Command Set) specification defines the standard command set (0x01-0x3F range)
- RP2040 PL022 SSP described in RP2040 Datasheet Section 4.4
