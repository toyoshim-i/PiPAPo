# calc — programmer's calculator

## Overview

**calc** is a PPAP-native interactive calculator modelled on physical
calculators (Casio fx Programmer mode, macOS Programmer Calc) rather
than line-oriented expression evaluators like `bc`.  A single keystroke
drives the display: type digits, press an operator, type more digits,
press `=`.

The killer feature is **base switching**: pressing `H` while a value is
displayed re-renders it in hex without recomputing — `65535` becomes
`FFFF` and subsequent input parses as hex.  The same is true for `D`
(decimal), `O` (octal), and `B` (binary).

The display is a 7-segment-style LED font (3 ASCII rows per digit) for
DEC / HEX / OCT and a row of LED dots (`*` lit, `.` unlit) for BIN.
A plain-text fallback kicks in when the LED form would not fit the
current TTY.  An optional tape pane on the right (≥ 80 cols) shows the
last several operations.

## Launching

```
calc
```

Calc reads no command-line arguments and writes nothing to standard
output until you quit.  It quits cleanly on `Q`, lowercase `q`, or
`Ctrl-D`, restoring the terminal mode and leaving the shell prompt on
a fresh line.

## Display layout

Calc draws a single ASCII frame around the whole UI.  Inside the frame
it stacks the following sections, top to bottom:

1. **Status line** — `<base>  W=<width>  signed/unsigned  [pending op]
   [M..]  [error]`.  The pending-op tag appears between operands; the
   `[M..]` tag appears while waiting for the second stroke of an
   `M`-prefix command.
2. **Display** — the current value.  3 rows in 7-seg / LED-dot mode,
   1 row in plain-text fallback, right-aligned within the inner area.
3. **`ans` and `M` line** — the last committed result and the memory
   register, both rendered in the current base.
4. **Tape line** (narrow TTYs only) — the most recent calculation.
5. **Three rows of key hints** — fixed-width 6-char cells, leading
   character drawn in reverse video to mark the hotkey.

On TTYs ≥ 80 cols a side **tape pane** is shown to the right of the
main column.  The single tape line at the bottom of the main column
disappears and the side pane carries the running history instead.

## Key bindings

The convention is symmetric:

- **Uppercase letters** (`A`-`Z`) — commands.  Every command's hotkey
  is the first letter of its hint label, drawn in reverse video.
- **Lowercase letters `a`-`f`** — hex digit input.  Only meaningful in
  HEX mode; ignored in DEC / OCT / BIN.
- **Digits `0`-`9`** — decimal digit input, filtered by current base.
- **Lowercase `q`** — quit (escape hatch following the vi / less
  convention).  All other lowercase letters that don't double as hex
  digits are no-ops by design.

### Digit input

| Key | Effect |
|---|---|
| `0`-`9` | Decimal digit.  Rejected if invalid for the current base (e.g. `8` in OCT). |
| `a`-`f` | Hex digit (HEX mode only).  Use lowercase — uppercase `A`-`F` collide with command keys. |
| Backspace | Delete the last entered digit. |

### Arithmetic and bitwise

| Key | Op | Notes |
|---|---|---|
| `+` `-` `*` `/` | add / sub / mul / divide | Integer divide; truncates toward zero. |
| `%` | modulo | |
| `&` `\|` `^` | bitwise AND / OR / XOR | `^` is XOR, **not** power. |
| `~` | bitwise NOT | Unary, applied to the current display value. |
| `<` `>` | shift left / shift right | Single keypress (not C `<<` / `>>`).  `>>` is arithmetic when signed-display is on, logical otherwise. |
| `=` or Enter | commit pending op | |

Operators have **no precedence** — entries are folded left-to-right
just like a physical calculator: `2 + 3 * 4 =` produces `20`, not `14`.

Divide-by-zero or modulo-by-zero clears the display, sets `err` on the
status line, and refuses further input until you press `C` (clear
entry).

### Mode and width

| Key | Action |
|---|---|
| `D` | Switch to DEC mode |
| `H` | Switch to HEX mode |
| `O` | Switch to OCT mode |
| `B` | Switch to BIN mode |
| `W` | Cycle width: 8 → 16 → 32 → 64 → 8 |
| `S` | Toggle signed / unsigned display |
| `N` | Negate (two's-complement) |

Width is a real register width, not just a display mask: at width=8,
`200 + 200` is `144` (`0x190 & 0xFF = 0x90`).  Reducing width masks
both the display and accumulator to the new width.

### Editing

| Key | Action |
|---|---|
| `C` | Clear entry — display = 0, err = none.  Pending op and accumulator preserved. |
| `R` | Reset all — clears display, accumulator, pending op, tape, and error.  Width / base / sign preferences are kept. |

### Display

| Key | Action |
|---|---|
| `T` | Toggle 7-seg / plain-text display.  Useful when the LED form would clip on a narrow TTY. |

### Memory register

The memory register is a single int64, masked to the current width.
All operations are two-stroke: press `M` (status shows `[M..]`), then
the lowercase action.

| Stroke | Action |
|---|---|
| `M` `s` | **Store** — `mem = display` |
| `M` `r` | **Recall** — `display = mem` |
| `M` `+` | **Add** — `mem = mem + display` |
| `M` `-` | **Subtract** — `mem = mem - display` |
| `M` `c` | **Clear** — `mem = 0` |

The current memory value is always shown next to `ans` in the
"M = …" field below the display.

### Help and quit

| Key | Action |
|---|---|
| `?` | Toggle the help overlay (full key reference; press any key to dismiss) |
| `Q` or `q` | Quit |
| `Ctrl-D` | Quit |

## Examples

### Compute `12 + 34` in decimal

```
1 2 + 3 4 =
```

`ans = 46`.

### Inspect `0xDEADBEEF` in different bases

```
H 0 x       (or just press 'H' to enter HEX mode, then type)
d e a d b e e f
```

The 7-seg display reads `DEAD_BEEF` (with a middle-row underscore
between the two 4-digit groups).  Press `D` to see `3,735,928,559` in
decimal commas, `B` for the 32-bit LED-dot row, `O` for octal.

### Mask the low byte of a value

```
H f f 0 0 a a b b   (display = 0xFF00AABB)
& 0 x f f =          (or just '& f f =', since base is HEX)
```

`ans = 0xBB`.

### Save an intermediate result, work on something else, recall

```
1 2 3 4 5 =      (display = 12345)
M s              (store mem = 12345)
9 9 9 + 1 =      (display = 1000)
... more work ...
M r              (display = 12345, recovered)
```

### Build a sum across several values

```
1 0 0 M +        (mem = 100)
2 5 0 M +        (mem = 350)
5 0 M -          (mem = 300)
M r              (display = 300)
```

## Display modes in detail

### 7-segment LED font (DEC / HEX / OCT)

Three ASCII rows per digit, abutting (no gap between digits).  Glyphs
for hex use lowercase `b` and `d` to disambiguate from `8` and `0`.
Grouping separators paint in a single column between groups:

- DEC: comma `,` at the bottom row, every 3 digits from the right
- HEX: underscore `_` at the middle row, every 4 digits
- OCT: underscore `_` at the middle row, every 3 digits

### LED dot row (BIN)

One cell per bit, lit (`*`) or unlit (`.`), with a single space between
every nibble.  Always padded to the full register width — width=32
shows all 32 bits, leading zeros included, since the visual point of
BIN mode is to see every bit.

### Plain-text fallback

When the LED form would overflow the available width, calc falls back
to a single line of plain text using the same grouped form as
`calc_render_grouped`:

```
1,234,567        (DEC)
0xDEAD_BEEF      (HEX)
0o12_345_670     (OCT)
0b1100_1100      (BIN)
```

Press `T` to toggle 7-seg / plain-text manually regardless of fit.

## Status line indicators

| Tag | Meaning |
|---|---|
| `DEC` `HEX` `OCT` `BIN` | Current base |
| `W=8` … `W=64` | Current width in bits |
| `signed` / `unsigned` | Display sign-interpretation |
| `[pending +]`, `[pending *]`, … | An operator is waiting for its right-hand operand |
| `[M..]` | `M` was pressed; awaiting the second stroke (`s`/`r`/`+`/`-`/`c`) |
| `ERR: divide by zero` | Last divide / modulo had a zero divisor.  Press `C` to clear. |

## Limitations

- **Integer only.**  No floating point or fractions.  Real-number
  support in DEC mode is filed as future work.
- **No expression parser.**  `12 + 34 * 56` is `(12 + 34) * 56`
  (left-to-right), not `12 + (34 * 56)`.
- **No transcendental functions** (`sin`, `cos`, `sqrt`, `log`).
  PPAP user space has no libm.
- **Single memory register.**  No `M0`-`M9` slot bank.
- **No history beyond the tape.**  Tape entries scroll off after 16
  operations and aren't recallable.
