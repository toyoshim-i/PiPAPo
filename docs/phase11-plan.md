# Phase 11: PicoCalc Device Support — Detailed Plan

**PicoPiAndPortable Development Roadmap**
Estimated Duration: 4 weeks
Prerequisites: Phase 10 (Stabilization) complete
**Status: Not started**

---

## Context

Phases 0–10 built a functional UNIX-like OS that runs busybox ash on UART.
All three build targets (`qemu`, `pico1`, `pico1calc`) share the same kernel,
but the PicoCalc's distinctive hardware — a 320×320 IPS LCD and a full QWERTY
keyboard — remains unused.  The user interacts via UART serial only.

This phase adds PicoCalc-specific peripheral drivers: the SPI1-connected LCD
display as a VT100-compatible text console, and the I2C-connected STM32
keyboard controller as the primary input device.  After this phase, `pico1calc`
boots to an interactive ash shell on the built-in screen and keyboard, with
UART available as a secondary debug console.

## Hardware Summary (from docs/PicoCalc.md)

**Display:**
- 4-inch 320×320 IPS LCD, ST7796S or ILI9488 controller
- Interface: SPI1 (GP10=SCK, GP11=MOSI, GP13=CS, GP14=DC, GP15=RST)
- 16-bit color (RGB565)

**Keyboard:**
- Full QWERTY, scanned by STM32 co-processor
- Interface: I2C1 (GP6=SDA, GP7=SCL), slave address 0x1F, 10 kHz
- Key events via FIFO registers (REG_ID_KEY=0x04, REG_ID_FIF=0x09)
- Also exposes: LCD backlight (0x05), keyboard backlight (0x0A),
  battery voltage (0x0B), power off (0x0E)

## Design Decisions

**No pixel framebuffer in SRAM.**  A 320×320×2 (RGB565) framebuffer would
consume 200 KB — nearly all of SRAM.  Instead, maintain a character-cell
buffer and render cells directly to the LCD via SPI1 on demand.  Only changed
cells are redrawn.  The cell buffer size depends on the active font mode
(see below).

**Switchable fonts: 8×16 (40×20) and 4×8 (80×40).**  Two built-in bitmap
fonts provide a tradeoff between readability and terminal size:

| Mode | Font | Grid | Flash (.rodata) | Cell buffer (SRAM) | Use case |
|------|------|------|------------------|--------------------|----------|
| Normal | 8×16 | 40×20 | 4 KB (256×16B) | 1.6 KB (800+800) | Shell, general use |
| Small | 4×8 | 80×40 | 2 KB (256×8B) | 6.4 KB (3200+3200) | Rogue, 80-col apps |

The 8×16 font is the default at boot.  Applications switch to 4×8 mode via
a custom escape sequence (`ESC [ ? 8 0 h` to enable 80-col mode, `ESC [ ? 8 0 l`
to return to 40-col).  The fbcon driver reallocates the cell buffer on switch,
clears the screen, and reports the new size via TIOCGWINSZ.

The 4×8 font produces characters 0.9mm × 1.8mm on the 4" display — tiny for
flowing text, but perfectly readable for Rogue's single-glyph map characters
(`@`, `#`, `.`, `A-Z`) where each symbol is visually distinct.  The full
80-column width means Rogue runs at its classic terminal size with zero
modification.

Total font data: 6 KB in flash.  Both font tables reside in .rodata (XIP).

**Row-based SPI rendering.**  Instead of per-cell SPI transactions (each
requiring a set-window command), render an entire row in one SPI burst.
One row = 320×16 pixels × 2 bytes = 10,240 bytes.  At 40 MHz SPI clock,
that's ~2 ms per row.  Full screen redraw ≈ 40 ms (~25 FPS), acceptable for
a text console.

**Console abstraction.**  A thin console backend interface decouples the TTY
layer from the physical device:

```c
typedef struct {
    void (*putc)(char c);           /* write one character */
    int  (*getc)(void);             /* read one character (-1 if none) */
    int  (*poll)(void);             /* non-zero if input available */
} console_ops_t;
```

On `pico1` and `qemu`, the backend is UART.  On `pico1calc`, the primary
backend is LCD+keyboard; UART is the secondary backend (mirrors output, not
polled for input unless no keyboard is detected).

**Keyboard polling in SysTick.**  The STM32 keyboard is polled every 20 ms
(every other SysTick tick) via I2C1.  Key events are translated to ASCII
(with modifier handling for Shift/Ctrl) and pushed into the TTY RX path,
identical to UART IRQ-driven input.  Arrow keys generate VT100 escape
sequences (\033[A/B/C/D).

**VT100 escape sequence parser.**  Required for busybox ash prompt, ls color
output, and cursor-based applications (vi, Rogue).  Implements the CSI
(ESC [) subset: cursor movement, erase line/screen, SGR color attributes
(8 basic ANSI colors foreground/background), and scroll region.

**QEMU and pico1 unaffected.**  All display/keyboard code is compiled only
into `ppap_pico1calc`.  The console abstraction ensures `qemu` and `pico1`
continue to use UART without code changes.  New target capability flags
`TARGET_CAP_DISPLAY` and `TARGET_CAP_KBD` are added.

## Exit Criteria

1. `scripts/test_all_targets.sh` builds all three targets (no regressions)
2. `scripts/qemu-test.sh` passes all existing tests (UART console unchanged)
3. On pico1calc: kernel boot messages appear on LCD
4. On pico1calc: ash prompt displayed, keyboard input accepted, commands execute
5. On pico1calc: `ls`, `cat`, `echo`, pipes render correctly on screen
6. VT100 cursor movement and clear-screen work (tested with Rogue or `vi`)
7. Ctrl-C from keyboard delivers SIGINT to foreground process
8. LCD backlight adjustable via `/dev/backlight` or ioctl
9. UART continues to work as secondary debug console on pico1calc

---

## Week 1 — Hardware Drivers

### Step 1 — I2C1 driver

**New files:** `src/drivers/i2c.c`, `src/drivers/i2c.h`

RP2040 I2C is Synopsys DesignWare DW_apb_i2c.  I2C1 base: `0x40048000`.

API:
```c
void i2c_init(void);                              /* 10 kHz, GP6/GP7 */
int  i2c_read_reg(uint8_t addr, uint8_t reg,
                  uint8_t *buf, size_t len);       /* master read */
int  i2c_write_reg(uint8_t addr, uint8_t reg,
                   const uint8_t *buf, size_t len);/* master write */
```

Implementation:
- Deassert I2C1 reset via RESETS register
- Configure GP6 (SDA) and GP7 (SCL) with FUNCSEL=3 (I2C)
- Set IC_CON: master mode, 7-bit addressing, restart enable
- Set IC_SS_SCL_HCNT / IC_SS_SCL_LCNT for 10 kHz @ 133 MHz peri clock
  (HCNT = 6650, LCNT = 6650 for ~10 kHz)
- Enable with IC_ENABLE = 1
- Set target address via IC_TAR before each transaction
- Read: write reg byte with RESTART, then issue read commands
- Write: write reg byte + data bytes
- Timeout: 100 ms spin-wait on IC_RAW_INTR_STAT

~100 ms post-init delay before first keyboard access (STM32 boot time).

### Step 2 — SPI1 driver for LCD

**New files:** `src/drivers/spi_lcd.c`, `src/drivers/spi_lcd.h`

SPI1 base: `0x4003D000` (PL022, same IP as SPI0).  Separate from SD card's
SPI0 — no contention.

API:
```c
void spi_lcd_init(void);                  /* 40 MHz, GP10/GP11 */
void spi_lcd_cmd(uint8_t cmd);            /* DC=0, send command byte */
void spi_lcd_data(const uint8_t *buf, size_t len);  /* DC=1, send data */
void spi_lcd_set_window(uint16_t x0, uint16_t y0,
                        uint16_t x1, uint16_t y1);  /* CASET+RASET */
void spi_lcd_write_pixels(const uint16_t *px, size_t count); /* stream */
```

GPIO setup:
- GP10 (SCK): FUNCSEL=1 (SPI1)
- GP11 (MOSI): FUNCSEL=1 (SPI1)
- GP13 (CS): FUNCSEL=5 (GPIO output, active low, manual control)
- GP14 (DC): FUNCSEL=5 (GPIO output, 0=command 1=data)
- GP15 (RST): FUNCSEL=5 (GPIO output, active low)

Clock: SPI1 prescaler for ~40 MHz (CPSDVSR=2, SCR=0 at 133 MHz peri clock
gives 66.5 MHz; SCR=1 gives 33.25 MHz — use SCR=1 for safe margin).

SPI1 is TX-only for the LCD (MISO not connected).  Configure PL022 in
transmit-only mode.

### Step 3 — LCD controller initialization

**New files:** `src/drivers/lcd.c`, `src/drivers/lcd.h`

Initialize the ST7796S (primary) or ILI9488 (variant) controller.  Both share
a nearly identical command set (MIPI DCS standard).

```c
void lcd_init(void);                       /* reset + init sequence */
void lcd_fill_rect(uint16_t x, uint16_t y,
                   uint16_t w, uint16_t h,
                   uint16_t color);        /* solid fill */
```

Init sequence:
1. Assert RST low 10 ms, release, wait 120 ms
2. Software Reset (0x01), wait 120 ms
3. Sleep Out (0x11), wait 120 ms
4. Pixel Format Set (0x3A): 0x55 (16-bit RGB565)
5. Memory Access Control (0x36): set rotation/mirror for correct orientation
   (depends on how the panel is mounted — may need 0x48 or 0x28)
6. Column/Row Address Set (0x2A/0x2B): 0–319 for both
7. Display On (0x29)

**Orientation note:** The ST7796S is typically a 320×480 controller.  The
PicoCalc uses a 320×320 panel, so we set the window to the top 320 rows.
The memory access control byte selects the correct scan direction.

Verification: `lcd_fill_rect(0, 0, 320, 320, 0x001F)` should fill the screen
blue.

### Step 4 — Keyboard polling driver

**New files:** `src/drivers/kbd.c`, `src/drivers/kbd.h`

```c
void kbd_init(void);        /* verify STM32 presence via REG_ID_VER */
int  kbd_poll(void);        /* returns ASCII char or -1; handles modifiers */
```

Polling procedure (per PicoCalc.md):
1. Read 1 byte from register 0x04 (REG_ID_KEY)
2. Extract FIFO count: `value & 0x1F`
3. If count > 0, read 2 bytes from register 0x09 (REG_ID_FIF)
   - Byte 0: state (1=pressed, 2=released, 3=held)
   - Byte 1: keycode
4. On key-press event, translate keycode to ASCII
5. Handle modifiers: Shift → uppercase/symbols, Ctrl → control codes (0x01–0x1A)
6. Arrow keys → VT100 sequences: Up=\033[A, Down=\033[B, Right=\033[C, Left=\033[D

Keycode translation table stored in .rodata (flash).

---

## Week 2 — Text Console

### Step 5 — Bitmap fonts (8×16 and 4×8)

**New files:**
- `tools/bdf2c.py` — host-side BDF→C converter
- `third_party/fonts/8x16.bdf` — original 8×16 font (with LICENSE)
- `third_party/fonts/4x6.bdf` — original 4×6 font (with LICENSE)
- Generated at build time: `${CMAKE_BINARY_DIR}/generated/font8x16.c`,
  `${CMAKE_BINARY_DIR}/generated/font4x8.c`

Two fixed-width bitmap fonts.  Original BDF files are stored under
`third_party/fonts/` alongside their license files for clear attribution.
C arrays are generated at build time by `tools/bdf2c.py` and are never
committed to the repository.

**Font source pipeline:**
```
third_party/fonts/8x16.bdf  →  bdf2c.py  →  ${BUILD}/generated/font8x16.c
third_party/fonts/4x6.bdf   →  bdf2c.py  →  ${BUILD}/generated/font4x8.c
```

CMake `add_custom_command()` runs `bdf2c.py` during build, producing the
generated C sources.  The pico1calc target adds them via
`target_sources(ppap_pico1calc PRIVATE ...)`.

```cmake
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/generated/font8x16.c
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/bdf2c.py
    --input ${CMAKE_SOURCE_DIR}/third_party/fonts/8x16.bdf
    --output ${CMAKE_BINARY_DIR}/generated/font8x16.c
    --name font8x16 --width 8 --height 16
  DEPENDS ${CMAKE_SOURCE_DIR}/third_party/fonts/8x16.bdf
          ${CMAKE_SOURCE_DIR}/tools/bdf2c.py
)
```

**`tools/bdf2c.py`:** Reads a BDF (Bitmap Distribution Format) file, extracts
glyph bitmaps for codepoints 0x00–0xFF, pads/crops to the target dimensions
(e.g., 4×6 → 4×8 with 1 row top/bottom padding), and emits a C source file
containing a `const uint8_t fontNxM[256][H]` array.

**Third-party font directory layout:**
```
third_party/fonts/
├── 8x16.bdf          # VGA BIOS ROM font (8×16, misc-fixed)
├── 4x6.bdf           # X11 misc-fixed 4×6 font
├── LICENSE            # Public domain / MIT (X.Org misc-fixed)
└── README.md          # Font provenance and upstream URLs
```

**8×16 (Normal mode, 40×20):** From the X11 misc-fixed collection
(`8x16.bdf`).  The classic VGA BIOS ROM font, public domain.  256 glyphs ×
16 bytes = 4 KB.  Each glyph is 16 bytes, one byte per row, MSB-first.

```c
extern const uint8_t font8x16[256][16];  /* 4 KB in .rodata */
```

**4×8 (Small mode, 80×40):** From X11 misc-fixed `4x6.bdf` (public domain,
X.Org).  `bdf2c.py` pads 4×6 glyphs to 4×8 (1 row top, 1 row bottom) for
improved vertical spacing.  256 glyphs × 8 bytes = 2 KB.  Each glyph is 8
bytes, one byte per row, top 4 bits used.

```c
extern const uint8_t font4x8[256][8];   /* 2 KB in .rodata */
```

Both font BDF files include box-drawing characters (U+2500 range mapped to
0xB0–0xDF) for Rogue's dungeon borders.

### Step 6 — Framebuffer text console core

**New files:** `src/drivers/fbcon.c`, `src/drivers/fbcon.h`

Character-cell text console with two modes: 40×20 (8×16 font) and 80×40
(4×8 font).

Data structures (in SRAM):
```c
/* Sized for largest mode (80×40) */
static uint8_t  cell_char[FBCON_MAX_ROWS][FBCON_MAX_COLS]; /* 3200 bytes */
static uint8_t  cell_attr[FBCON_MAX_ROWS][FBCON_MAX_COLS]; /* 3200 bytes */
static uint8_t  dirty[FBCON_MAX_ROWS];                     /* 40 bytes */
static int      cursor_x, cursor_y;
static int      cols, rows;               /* active grid size */
static const uint8_t (*font)[16];         /* pointer to active font */
static int      font_w, font_h;           /* 8×16 or 4×8 */
/* Total SRAM: ~6.5 KB (allocated for max mode at all times) */
```

The cell buffers are statically allocated at the 80×40 maximum.  In 40×20
mode, only the top-left portion is used.  This avoids dynamic allocation
and simplifies mode switching.

`cell_attr` encodes foreground (bits 0–3) and background (bits 4–7) color
indices into a 16-entry RGB565 palette (standard ANSI colors).

API:
```c
void fbcon_init(void);                /* clear screen, set cursor to 0,0 */
void fbcon_putc(char c);              /* process one character */
void fbcon_flush(void);               /* redraw dirty rows */
void fbcon_clear(void);               /* clear all cells */
void fbcon_set_cursor(int x, int y);  /* move cursor */
```

`fbcon_putc()` handles:
- Printable chars: write to cell buffer, advance cursor, mark row dirty
- `\n`: move to next line, scroll if at bottom
- `\r`: cursor to column 0
- `\b`: move cursor left
- `\t`: advance to next 8-column tab stop
- Scroll: memmove cell buffer up one row, clear bottom row, mark all dirty

`fbcon_flush()` renders dirty rows via `lcd_set_window()` + SPI1 pixel stream.
Row rendering: for each cell in the row, look up font bitmap, apply fg/bg
colors, emit 8×16 RGB565 pixels.  Uses a 640-byte row pixel buffer
(320 pixels × 2 bytes) rendered line-by-line (16 SPI transactions per row).

Cursor rendering: invert the cell at cursor position.  Toggle on periodic
callback (500 ms blink rate) via a flag checked in `fbcon_flush()`.

### Step 7 — VT100 escape sequence parser

**Integrated into:** `src/drivers/fbcon.c`

State machine for ANSI/VT100 CSI sequences (ESC [ ... final_byte):

```
States: NORMAL → ESC_SEEN → CSI_PARAM → CSI_DONE
```

Supported sequences:
| Sequence | Meaning |
|----------|---------|
| `ESC [ n A` | Cursor up n rows |
| `ESC [ n B` | Cursor down n rows |
| `ESC [ n C` | Cursor right n columns |
| `ESC [ n D` | Cursor left n columns |
| `ESC [ r ; c H` | Cursor position (row; col) |
| `ESC [ J` | Erase below cursor |
| `ESC [ 2 J` | Erase entire screen |
| `ESC [ K` | Erase to end of line |
| `ESC [ n m` | SGR: set color/attribute |
| `ESC [ ? 25 h/l` | Show/hide cursor |
| `ESC [ ? 80 h` | Switch to 80-col mode (4×8 font, 80×40) |
| `ESC [ ? 80 l` | Switch to 40-col mode (8×16 font, 40×20) |
| `ESC [ n ; n r` | Set scroll region (for Rogue) |

The `? 80` private mode is modeled after xterm's DECCOLM (mode 3) but uses
a private number to avoid conflict.  On mode switch, the screen is cleared,
cursor reset to (0,0), and SIGWINCH is delivered to the foreground process
group so that applications (ash, Rogue) query the new TIOCGWINSZ.

SGR color mapping: 30–37 foreground, 40–47 background, 0 reset, 1 bold
(map to bright color variant).

This is sufficient for busybox ash, ls --color, vi, and Rogue.

### Step 8 — /dev/tty1 and /dev/console integration

**File:** `src/kernel/fs/devfs.c`

Add device nodes:
- `/dev/tty1` — LCD+keyboard console (read → kbd, write → fbcon)
- `/dev/console` — alias to tty1 on pico1calc (was alias to ttyS0)

devfs callbacks:
```c
static long devtty1_read(struct file *f, char *buf, size_t n);
static long devtty1_write(struct file *f, const char *buf, size_t n);
static int  devtty1_ioctl(struct file *f, uint32_t cmd, void *arg);
static int  devtty1_poll(struct file *f);
```

`devtty1_write()` calls `fbcon_putc()` for each byte, then `fbcon_flush()`.
`devtty1_read()` returns characters from the keyboard FIFO (blocks if empty).
`devtty1_ioctl()` handles TIOCGWINSZ (returns 40×20 or 80×40 depending on
active font mode), termios get/set.

On `pico1calc`, init's stdin/stdout/stderr opens `/dev/console` → tty1.
The TTY line discipline layer (tty.c) processes input identically whether
sourced from UART or keyboard.

---

## Week 3 — Integration

### Step 9 — Keyboard → TTY input path

**Files:** `src/kernel/fd/tty.c`, `src/kernel/proc/sched.c`

Keyboard polling hook in `sched_tick()` (SysTick handler), every other tick
(20 ms):

```c
if (target_caps() & TARGET_CAP_KBD) {
    int ch = kbd_poll();
    if (ch >= 0)
        tty_rx_char(ch);   /* feed into TTY layer */
}
```

`tty_rx_char()` is a new function in tty.c that processes a single input
character through the line discipline (same as UART IRQ path): echo, canonical
buffering, ISIG (Ctrl-C → SIGINT, Ctrl-Z → SIGTSTP), Ctrl-D → EOF.

For multi-byte sequences (arrow keys), `kbd_poll()` returns -1 between bytes;
the keyboard driver internally buffers the 3-byte VT100 sequence and returns
one byte per call.

### Step 10 — Console backend abstraction

**New file:** `src/drivers/console.c`, `src/drivers/console.h`

```c
typedef struct {
    void (*putc)(char c);
    int  (*getc)(void);
    int  (*poll)(void);
    void (*flush)(void);
} console_ops_t;

void console_register(const console_ops_t *primary,
                      const console_ops_t *secondary);
void console_putc(char c);     /* writes to primary + secondary */
int  console_getc(void);       /* reads from primary only */
```

On `pico1calc`:
- Primary: LCD+kbd (`fbcon_putc`, `kbd_poll`)
- Secondary: UART (`uart_putc`, NULL) — output mirrored, no input

On `pico1` and `qemu`:
- Primary: UART
- Secondary: NULL

Kernel `kprintf()` and boot messages use `console_putc()`, so they appear on
both LCD and UART on pico1calc.

**Files modified:** `src/kernel/klog.c` (switch from direct `uart_putc` to
`console_putc`), `src/target/pico1calc/target_pico1calc.c` (register backends
in `target_early_init`).

### Step 11 — LCD backlight control

**File:** `src/kernel/fs/devfs.c`

Add `/dev/backlight` device node:
- Write an ASCII number 0–255 to set brightness
- Read returns current brightness
- Implementation: `i2c_write_reg(0x1F, 0x05, &val, 1)`

Default brightness set to 128 at boot in `target_late_init()`.

### Step 12 — Battery and power status

**File:** `src/kernel/fs/procfs.c`

Add `/proc/battery` entry:
```
voltage: 3847 mV
percentage: 72%
```

Read via `i2c_read_reg(0x1F, 0x0B, ...)` in procfs read callback.

Power off: add `SYS_REBOOT` syscall with `RB_POWER_OFF` option that writes
to REG_ID_OFF (0x0E).  Or expose via `/dev/power` with write "off".

---

## Week 4 — Polish and Hardware Verification

### Step 13 — Scroll performance optimization

**File:** `src/drivers/fbcon.c`

Current scroll: memmove cell buffer + mark all 20 rows dirty → full redraw
(~40 ms).  Optimizations:

1. **Dirty-cell tracking**: Instead of per-row dirty bits, use per-cell dirty
   bits (800 bits = 100 bytes).  Only redraw changed cells during normal
   output.  Scroll still marks all dirty.

2. **Row pixel buffer**: Pre-render one row of pixels (320×16×2 = 10 KB).
   This is too large for SRAM.  Instead, render and send one scanline at a
   time: 320×2 = 640 bytes buffer, sent 16 times per row.

3. **Hardware scroll** (if supported): ST7796S supports Vertical Scroll
   Definition (0x33) + Vertical Scroll Start Address (0x37).  This shifts
   the display start line without rewriting pixel data — only the new
   bottom row needs rendering.  Reduces scroll cost from ~40 ms to ~2 ms.

### Step 14 — Terminal compatibility testing

**On pico1calc hardware:**

- Boot message rendering
- ash prompt with line editing (backspace, arrow keys, history)
- `ls` with colored output, `cat` file display, `echo` with special chars
- Multi-line output scrolling
- `grep --color` highlighting
- Pipe rendering (`ls | head`)
- Ctrl-C interrupt during `sleep` or `cat`
- Ctrl-D to exit shell

### Step 15 — `ttyctl` utility

**New file:** `apps/ttyctl/ttyctl.c`
**Installed to:** `/usr/bin/ttyctl` (pico1calc romfs only)

A target-specific terminal management CLI for the PicoCalc display console.
Statically linked standalone binary (not a busybox applet), compiled for
Thumb and placed in the pico1calc romfs at `/usr/bin/ttyctl`.

Usage:
```
ttyctl reset          # Reset all terminal state to defaults:
                      #   - 40×20 mode (8×16 font)
                      #   - default fg/bg colors (white on black)
                      #   - clear screen, cursor to (0,0)
                      #   - cursor visible, no scroll region
                      #   - canonical echo mode restored
ttyctl 80             # Switch to 80-col mode (4×8 font, 80×40)
ttyctl 40             # Switch to 40-col mode (8×16 font, 40×20)
ttyctl cols           # Print current column count (for scripts)
ttyctl backlight N    # Set LCD backlight brightness (0–255)
ttyctl battery        # Print battery voltage and percentage
ttyctl poweroff       # Power off the device
```

Implementation: each subcommand emits the appropriate escape sequences to
stdout or reads/writes the relevant `/dev/backlight` and `/proc/battery`
files.  The `reset` subcommand emits: `ESC c` (RIS — full reset),
`ESC [ ? 80 l` (40-col mode), `ESC [ 0 m` (default colors),
`ESC [ 2 J` (clear screen), `ESC [ H` (cursor home).

This is useful for recovering from programs that crash mid-escape-sequence,
or from scripts that need to query/set the display mode.

**Build:** Standalone C source, compiled with `arm-none-eabi-gcc -mcpu=cortex-m0plus
-mthumb -Os -static`.  Added to pico1calc romfs via mkromfs.
CMake target: `ttyctl` in `apps/ttyctl/CMakeLists.txt`.

### Step 16 — Rogue on display (font switching verification)

Verify Rogue 5.4.4 renders correctly on the PicoCalc display:

- Rogue's startup script (or curses shim init) emits `ESC [ ? 80 h` to
  switch to 80×40 mode before launching
- Dungeon map at full 80-column width — classic layout, no compromise
- Box-drawing characters for room borders
- Cursor positioning via VT100 sequences
- Color attributes for monsters/items
- Keyboard input for movement and commands (hjkl, arrow keys)
- On exit, emit `ESC [ ? 80 l` to restore 40×20 shell mode

### Step 17 — Documentation and version bump

**Files:**
- `src/kernel/fs/procfs.c` — bump version to "0.11.0"
- `src/target/target.h` — add `TARGET_CAP_DISPLAY`, `TARGET_CAP_KBD`
- `src/target/pico1calc/pico1calc.h` — add SPI1 + I2C1 pin definitions
- `src/target/pico1calc/CMakeLists.txt` — add new driver sources
- `docs/PicoPiAndPortable-spec-v06.md` or v07 — update Phase 11 status

---

## Files Modified (Complete)

| File | Change |
|------|--------|
| NEW `src/drivers/i2c.c` | I2C1 master driver (DW_apb_i2c, 10 kHz) |
| NEW `src/drivers/i2c.h` | I2C API |
| NEW `src/drivers/spi_lcd.c` | SPI1 driver for LCD (PL022, ~33 MHz) |
| NEW `src/drivers/spi_lcd.h` | SPI LCD API |
| NEW `src/drivers/lcd.c` | ST7796S/ILI9488 init + drawing primitives |
| NEW `src/drivers/lcd.h` | LCD API |
| NEW `src/drivers/kbd.c` | STM32 keyboard polling + keycode translation |
| NEW `src/drivers/kbd.h` | Keyboard API |
| NEW `tools/bdf2c.py` | Host tool: BDF → C array converter |
| NEW `third_party/fonts/8x16.bdf` | VGA ROM 8×16 font (public domain) |
| NEW `third_party/fonts/4x6.bdf` | X11 misc-fixed 4×6 font (public domain) |
| NEW `third_party/fonts/LICENSE` | Font license (public domain / MIT) |
| NEW `third_party/fonts/README.md` | Font provenance and upstream URLs |
| GEN `${BUILD}/generated/font8x16.c` | 8×16 font C array (build-time generated, not committed) |
| GEN `${BUILD}/generated/font4x8.c` | 4×8 font C array (build-time generated, not committed) |
| NEW `src/drivers/fbcon.c` | Framebuffer text console (40×20) + VT100 parser |
| NEW `src/drivers/fbcon.h` | Fbcon API |
| NEW `src/drivers/console.c` | Console backend abstraction |
| NEW `src/drivers/console.h` | Console API |
| NEW `apps/ttyctl/ttyctl.c` | Terminal management CLI (reset, mode switch, backlight) |
| NEW `apps/ttyctl/CMakeLists.txt` | Build config for ttyctl |
| `src/kernel/fs/devfs.c` | Add /dev/tty1, /dev/backlight |
| `src/kernel/fd/tty.c` | Add tty_rx_char() for keyboard input path |
| `src/kernel/proc/sched.c` | Keyboard poll hook in sched_tick() |
| `src/kernel/klog.c` | Use console_putc() instead of uart_putc() |
| `src/kernel/fs/procfs.c` | Add /proc/battery; version bump |
| `src/target/target.h` | TARGET_CAP_DISPLAY, TARGET_CAP_KBD |
| `src/target/pico1calc/pico1calc.h` | SPI1 + I2C1 pin definitions |
| `src/target/pico1calc/target_pico1calc.c` | Init I2C1, SPI1, LCD, kbd; register console |
| `src/target/pico1calc/CMakeLists.txt` | Add new driver sources |

## SRAM Budget

| Component | Size | Notes |
|-----------|------|-------|
| Cell buffer (chars) | 3,200 B | 80×40 (max mode) |
| Cell buffer (attrs) | 3,200 B | 80×40 (max mode) |
| Dirty flags | 40 B | Per-row, 80×40 |
| Scanline render buf | 640 B | 320 pixels × 2 bytes RGB565 |
| Keyboard state | 32 B | Modifier flags, FIFO staging |
| Console ops struct | 32 B | Two pointers |
| VT100 parser state | 16 B | State machine + param buffer |
| I2C driver state | 8 B | Minimal |
| **Total** | **~7.2 KB** | Fits in kernel data region |

The cell buffers are sized for the 80×40 maximum even when running in 40×20
mode.  This is 4.8 KB more than the 40×20-only design, but avoids dynamic
reallocation.  The kernel data region (20 KB) has sufficient headroom.

## Risks and Mitigations

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| LCD orientation wrong (mirror/flip) | High | Memory Access Control (0x36) has 8 variants; test each |
| ST7796S vs ILI9488 init differences | Medium | Both use MIPI DCS; differences are in gamma/voltage — use safe defaults |
| I2C 10 kHz too slow for key repeat | Low | 10 kHz is ~2 bytes/ms; one poll = 3 bytes ≈ 3 ms; 20 ms interval is fine |
| 4×8 font too small for some users | Low | Default is 8×16; 4×8 is opt-in for 80-col apps |
| Scroll flicker | Medium | Hardware scroll via ST7796S 0x33/0x37 eliminates full redraw |
| SPI1 contention with SPI0 (SD) | None | Separate peripherals, separate GPIO pins |
| Keyboard polling in SysTick adds latency | Low | I2C read ≈ 3 ms at 10 kHz; acceptable in tick handler |
| I2C bus hang (STM32 NAK or stretch) | Medium | Timeout + bus recovery (toggle SCL 9 times) |

## Deferred to Phase 12+

| Issue | Reason |
|-------|--------|
| /dev/fb0 raw framebuffer device | Text console sufficient; pixel API adds complexity |
| Multiple virtual consoles (Alt-F1/F2) | Single console sufficient for RP2040 memory |
| DMA for SPI1 transfers | Polling SPI is fast enough at 33 MHz for text console |
| PSRAM as extended page pool | Phase 12 (RP2350) scope; PIO driver needed |
| Audio / PWM speaker driver | Low priority; not needed for shell/Rogue |
| Keyboard backlight control | Nice-to-have; trivial to add later |