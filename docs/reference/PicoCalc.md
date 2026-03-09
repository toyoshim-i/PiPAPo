# ClockworkPi PicoCalc Hardware Specifications

This document summarizes the hardware specifications and development information for the ClockworkPi PicoCalc.

## Hardware Overview

- **Core Module**: Raspberry Pi Pico (RP2040) or Pico 2 (RP2350).
- **Display**: 4-inch 320x320 IPS LCD (Color).
  - Controller: ST7365P (Sitronix, ILI9488-compatible). See [PicoCalc-LCD.md](PicoCalc-LCD.md).
  - Interface: SPI1 (TX-only, no MISO line).
  - SPI Clock: ~33 MHz (133 MHz / 4, CPSDVSR=2, SCR=1).
  - Pixel Format: RGB565 (16-bit, 65K colours).
  - Panel is 320x480 controller constrained to top 320 rows.
- **Keyboard**: Full QWERTY physical keyboard.
  - Controller: STM32 (connected via I2C).
  - Key Scanning: Managed by STM32, Pico polls key events via I2C.
- **Storage**: Full-size SD card slot.
- **Memory**: 264KB SRAM (RP2040) / 520KB (RP2350) + 8MB PSRAM (carrier board).
- **Audio**: Dual amplified speakers.
- **Power**: USB-C or internal 18650 Li-ion batteries.

## Pinout Mapping (RP2040/RP2350)

| Interface | Signal | GPIO Pin | Notes |
|-----------|--------|----------|-------|
| **SPI1**  | SCK    | GP10     | LCD Clock |
| **SPI1**  | MOSI   | GP11     | LCD Data |
| **GPIO**  | CS     | GP13     | LCD Chip Select (Verified) |
| **GPIO**  | DC     | GP14     | LCD Data/Command (Verified) |
| **GPIO**  | RST    | GP15     | LCD Reset (Verified) |
| **UART0** | TX     | GP0      | Serial TX (To Southbridge/USB-C) |
| **UART0** | RX     | GP1      | Serial RX (To Southbridge/USB-C) |
| **UART1** | TX     | GP8      | To M_UART3_RX |
| **UART1** | RX     | GP9      | From M_UART3_TX |
| **I2C1**  | SDA    | GP6      | Keyboard/Southbridge SDA |
| **I2C1**  | SCL    | GP7      | Keyboard/Southbridge SCL |
| **SPI0**  | MISO   | GP16     | SD Card Data Out |
| **GPIO**  | CS     | GP17     | SD Card Chip Select |
| **SPI0**  | SCK    | GP18     | SD Card Clock |
| **SPI0**  | MOSI   | GP19     | SD Card Data In |
| **GPIO**  | CD     | GP22     | SD Card Detect (active low) |
| **PIO**   | SIO0   | GP2      | PSRAM Data 0 |
| **PIO**   | SIO1   | GP3      | PSRAM Data 1 |
| **PIO**   | SIO2   | GP4      | PSRAM Data 2 |
| **PIO**   | SIO3   | GP5      | PSRAM Data 3 |
| **PIO**   | CS     | GP20     | PSRAM Chip Select |
| **PIO**   | SCK    | GP21     | PSRAM Clock |
| **PWM**   | L      | GP26     | Audio Left Speaker |
| **PWM**   | R      | GP27     | Audio Right Speaker |
| **GPIO**  | LED    | GP25     | On-board LED |

## Serial Debugging

The PicoCalc uses the **STM32 (Southbridge)** as a USB-to-UART bridge.
- **Port**: USB-C
- **RP2040 Pins**: UART0 (TX: GP0, RX: GP1)
- **Settings**: 115200 baud, 8-bit, 1 stop bit, no parity (8N1).
- **USB Serial Name (Mac)**: `/dev/tty.usbserial-21440` (or similar).

### Connection Example
```bash
screen /dev/tty.usbserial-21440 115200
```

## I2C Protocol (Southbridge / STM32)
- **Slave Address**: `0x1F` (STM32 Co-processor)
- **I2C Bus**: `i2c1` (GP6: SDA, GP7: SCL)
- **Bus Speed**: `10kHz` (Critical for stability with STM32)

### Registers Map
| ID | Name | R/W | Description |
|---|---|---|---|
| `0x01` | `REG_ID_VER` | R | Firmware Version |
| `0x02` | `REG_ID_CFG` | R/W | Configuration (Ints, Overflow, Mods) |
| `0x03` | `REG_ID_INT` | R/W | Interrupt Status |
| `0x04` | `REG_ID_KEY` | R | Key FIFO Status (Bits 0-4: Count, 5: Caps, 6: Num) |
| `0x05` | `REG_ID_BKL` | R/W | LCD Backlight (0-255) |
| `0x06` | `REG_ID_DEB` | R/W | Debounce Configuration |
| `0x07` | `REG_ID_FRQ` | R/W | Polling Frequency |
| `0x08` | `REG_ID_RST` | W | Reset (Delay + NVIC Reset) |
| `0x09` | `REG_ID_FIF` | R | FIFO Data (Read 2 bytes: State, KeyCode) |
| `0x0A` | `REG_ID_BK2` | R/W | Keyboard Backlight (0-255) |
| `0x0B` | `REG_ID_BAT` | R | Battery Voltage/Percentage |
| `0x0C` | `REG_ID_C64_MTX` | R | C64 Matrix State |
| `0x0D` | `REG_ID_C64_JS` | R | C64 Joystick Bits |
| `0x0E` | `REG_ID_OFF` | W | Power Off |

### Keyboard Polling Procedure
1. **Read Status**: Read 1 byte from `REG_ID_KEY` (`0x04`) to check FIFO count.
2. **Check Count**: Extract count from `value & 0x1F`.
3. **Read FIFO**: If count > 0, read 2 bytes from `REG_ID_FIF` (`0x09`).
   - Byte 0: Key State (`1`=Pressed, `2`=Hold/Repeat, `3`=Released)
   - Byte 1: Key Code (ASCII 0x20-0x7E for printable, special codes below)
4. **Repeat**: Repeat until FIFO is empty.

### Key Codes
The STM32 handles Shift and produces ASCII directly for printable keys.
Special keycodes (from `keyboard.h` in STM32 firmware):

| Code | Key | Code | Key |
|------|-----|------|-----|
| `0x08` | Backspace | `0x09` | Tab |
| `0x0A` | Enter | `0x81-0x90` | F1-F10 |
| `0x91` | Power | `0xA1` | Alt |
| `0xA2` | Left Shift | `0xA3` | Right Shift |
| `0xA4` | Sym | `0xA5` | Ctrl |
| `0xB1` | Escape | `0xB4` | Left Arrow |
| `0xB5` | Up Arrow | `0xB6` | Down Arrow |
| `0xB7` | Right Arrow | `0xC1` | Caps Lock |
| `0xD0` | Break | `0xD1` | Insert |
| `0xD2` | Home | `0xD4` | Delete |
| `0xD5` | End | `0xD6` | Page Up |
| `0xD7` | Page Down | | |

### Hardware Details
- **Pull-ups**: Mainboard V2.0 has 4.7kΩ external pull-up resistors on GP6/GP7.
- **Initialization**: No hardware reset pin for STM32. Add ~100ms delay after I2C init.

## LCD Initialization

The LCD controller is an ST7365P (Sitronix), marketed as ILI9488-compatible.
It uses MIPI DCS commands plus vendor-specific extensions that require an
unlock sequence. The actual initialization is an 18-step sequence including
vendor command unlock, power control, gamma correction, and display inversion.

See [PicoCalc-LCD.md](PicoCalc-LCD.md) for the full initialization sequence,
SPI transport details, and framebuffer console architecture.

Key differences from a naive MIPI DCS init:
- **Vendor unlock required**: CMD `0xF0` with keys `0xC3` and `0x96` to enable
  RGB565 over 4-wire SPI (without this, COLMOD `0x55` is silently ignored).
- **MADCTL**: `0x48` (MX | BGR) — MX mirrors for panel orientation, BGR for
  the ST7365P's native subpixel order.
- **Display inversion on** (CMD `0x21`) is required for correct colour polarity.

## SD Card Interface
- **SPI Bus**: `spi0` (hardware SPI0 peripheral)
- **Pins**: MISO=GP16, CS=GP17 (GPIO), SCK=GP18, MOSI=GP19
- **Card Detect**: GP22 (GPIO input, active low — low when card inserted)
- **Init Clock**: ≤400 kHz (SD spec requires slow clock for CMD0)
- **Data Clock**: Up to 25 MHz (SD SPI mode maximum)
- **Protocol**: SD SPI mode (CMD0 with CS low to enter SPI mode)
- **Block Size**: 512 bytes (standard sector size)

### Sources
Pin assignments verified from:
- [FUZIX patch](https://github.com/clockworkpi/PicoCalc/tree/master/Code/FUZIX) (`CONFIG_PICOCALC` defines)
- [Picocalc_SD_Boot bootloader](https://github.com/adwuard/Picocalc_SD_Boot)
- [ClockworkPi Forum GPIO reference](https://forum.clockworkpi.com/t/gpio-for-pico-calc-how-to-make-firmware-for-pico-calc/20905/6)

## PSRAM Interface
- **Interface**: PIO (not hardware SPI) via `rp2040-psram` library on `pio1`
- **Pins**: CS=GP20, SCK=GP21, SIO0=GP2, SIO1=GP3, SIO2=GP4, SIO3=GP5
- **Capacity**: 8 MB (QSPI, but PIO driver uses single-bit SPI)
- **Library**: [rp2040-psram](https://github.com/polpo/rp2040-psram)

## Useful Links
- [Official ClockworkPi PicoCalc Schematics](https://github.com/clockworkpi/PicoCalc/tree/master/schematics)
- [STM32 Co-processor Firmware Source](https://github.com/clockworkpi/PicoCalc/tree/master/Code/picocalc_keyboard)
- [Pico SDK (C/C++)](https://github.com/raspberrypi/pico-sdk)
- [picocalc-text-starter](https://github.com/blairleduc/picocalc-text-starter): Minimal C++ framework for text-based apps.
