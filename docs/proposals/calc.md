# Proposal: `calc` — Interactive Programmer's Calculator

## Summary

A new PPAP-native interactive calculator app, modeled on physical
calculators (Casio fx / macOS Programmer mode) rather than expression
evaluators (`bc`, Python REPL).  The user types digits and operator
keys; a single accumulator and one pending operation drive the state
machine.  The display always shows the current value rendered in the
currently-selected base (DEC / HEX / OCT / BIN), and pressing a
mode-switch key re-renders the same underlying value in the new base
and changes input parsing accordingly — the killer programmer feature.

Phase 1 is **integer-only** with width-aware two's-complement wrapping
(8 / 16 / 32 / 64 bits).  Real-number support is reserved for a future
phase, restricted to DEC mode.

The layout is responsive to TTY size: a minimum view fits PicoCalc's
40×20 console, and wider TTYs (serial, QEMU, PicoCalc 80×40) gain a
"tape" pane that shows recent operations.  A future semi-graphic mode
(custom glyph upload via `fb_con`) is explicitly out of scope here.

Normative references for implementers:

- [docs/user/userland_dev_guide.md](/docs/user/userland_dev_guide.md)
  — compiler flags, linking, memory layout, ELF loader contract.
- [src/user/README.md](/src/user/README.md) — directory layout and
  "Adding a New Program" checklist.
- [docs/proposals/more_userland_apps.md](/docs/proposals/more_userland_apps.md)
  — overall direction for native userland apps.
- [docs/getting_started/coding_rules.md](/docs/getting_started/coding_rules.md)
  — style and commit-message rules.

## Motivation

- **End-user feature, not an internal tool.**  PPAP currently has no
  calculator at all.  A calculator is one of the first apps a user
  reaches for on any real computer; for PicoCalc — whose name and
  hardware *literally* point at this — its absence is conspicuous.
- **Programmer-focused identity.**  PPAP's audience is people who
  write C and read flash dumps.  A calc that lets you flip 65535
  between `65,535`, `0xFFFF`, `0b1111_1111_1111_1111`, and
  `0o177_777` with one key, and that emulates a real register width,
  is more useful than a generic four-banger.
- **Native, no busybox dependency.**  Busybox does not ship a
  calculator applet that fits this UX (`bc`/`dc` are line-oriented
  expression evaluators).  This is genuinely new functionality, not
  a busybox replacement.
- **Demonstrates TTY-responsive UI patterns.**  The size-adaptive
  layout (40×20 → wider) is a reusable template for other PPAP-native
  interactive apps (`pi`, `pile`, future semi-graphic apps).

## Non-Goals

- **Expression evaluator.**  No precedence parser, no `12+34*56`
  one-shot evaluation.  This is a button-calc, not `bc`.  An
  expression mode could be added later (`:e <expr>`) without
  disturbing the state machine, but it is not in scope.
- **libm / transcendental functions.**  No `sin`, `cos`, `sqrt`,
  `log`.  PPAP user-space has no libm and no plan to add one for
  this proposal.
- **Real numbers in Phase 1.**  Floating point and decimal
  fractions are deferred to a later phase, restricted to DEC mode.
  Phase 1 is purely integer.
- **Semi-graphic mode.**  Custom-glyph upload via `fb_con` is a
  separate, much larger piece of work and is filed as future work,
  not part of this proposal.
- **Memory registers (`M+ M- MR MC`).**  `ans` covers the common
  case; explicit memory keys add modal state for marginal value.
  Reconsider on demand.
- **Named variables (`A..Z`).**  Doesn't fit the button-calc model;
  belongs with a future expression mode.

## Design

### Internal model

A small state machine:

```
display    int64_t     value currently shown / being typed
accum      int64_t     left operand of pending op
pending    enum op     operation waiting for the next operand
entry      bool        true while user is typing into display
base       enum {DEC, HEX, OCT, BIN}
width      enum {W8, W16, W32, W64}
signed_    bool        affects display rendering only
ans        int64_t     last committed result
```

Pressing a digit:
- if `entry == false`, clear `display` and set `entry = true`
- if the digit is invalid for `base`, ignore (no beep, no error)
- otherwise shift `display` by the base and add the digit, then
  apply width mask

Pressing an operator `op`:
- if `pending != NONE`, fold: `display = apply(accum, pending, display)`
  with width-mask wrapping
- `accum = display`, `pending = op`, `entry = false`

Pressing `=` (Enter):
- if `pending != NONE`, fold as above
- `pending = NONE`, `ans = display`, `entry = false`

Pressing a base-switch key:
- re-render `display` in the new base; input parsing follows
- the underlying value is unchanged

Width change:
- mask `display` and `accum` to the new width (e.g. width 8 keeps the
  low 8 bits)

Sign toggle:
- display only; the underlying bit pattern is unchanged

### Operators

Arithmetic:  `+  -  *  /  %`
Bitwise:     `&  |  ^  ~  <<  >>`
Unary:       `+/-` (negate), `~` (bitwise not), `C` (clear entry),
             `AC` (clear all)

`/` and `%` are integer divide / modulo.  Divide-by-zero clamps to 0
and flashes an error indicator on the status line; the calc does not
crash or exit.

Width semantics: every op's result is masked to the current width.
For example, with `width = 8`:

```
200 + 200 = 144      (0xC8 + 0xC8 = 0x190, masked to 0x90)
~0        = 255      (signed view: -1)
1 << 9    = 0
```

`>>` is logical when `signed_ == false`, arithmetic when
`signed_ == true`.

### Number rendering

The display uses a **7-segment LED-style ASCII font** for the main
value when the rendered form fits the available width, falling back
to plain text otherwise.  This sells the physical-calc identity and,
on color-capable TTYs (e.g. PicoCalc `fb_con`), lit segments can be
drawn in red/amber for an authentic LED look.

#### 7-seg font

Three lines tall, ~4 columns per digit (3 glyph + 1 gap):

```
 _   _       _      _   _
|_|  _|  |  |_|    |_  |_|
| | |_   |  | |    |_| | |
```

Glyph table covers `0`–`9`, hex `A`–`F` (using lowercase `b` and
`d` to disambiguate from `8` and `0`), unary `-`, and a thin
separator glyph for comma/underscore grouping.  The whole table is
small (~16 entries × 3 lines × 3 chars ≈ 150 bytes of `const`).

#### Per-base behavior

| Base | 7-seg form               | Notes                          |
| ---- | ------------------------ | ------------------------------ |
| DEC  | digits + `-` sign        | comma grouping in 7-seg uses a thin separator glyph (single `,` between digit cells, not a full digit-width column) |
| HEX  | digits + `A`–`F` glyphs  | `_` grouping every 4           |
| OCT  | digits                   | `_` grouping every 3           |
| BIN  | **LED-dot row**, not 7-seg | each bit drawn as `●`/`○` (or `*`/`.` ASCII fallback), grouped per nibble — see below |

For BIN, the 7-seg of `0`/`1` looks degenerate and the value is too
wide anyway.  Instead, render the bits as a single-line LED dot row,
one cell per bit, grouped per nibble:

```
●●●● ●●●● ○○○○ ○○○○      ← 0xFF00, 16-bit width
```

This actually *gains* clarity over plain `0b...` text, since each
bit's lit/unlit state is visually scannable.

#### Width / fit fallback

The 7-seg form is used only when the rendered string fits the
available cols.  Otherwise the renderer falls back to plain text
(see below) on a single line.  Approximate fit for a 40-col TTY:

| Width | DEC (7-seg) | HEX (7-seg) | OCT (7-seg) | BIN (LED dots) |
| ----- | ----------- | ----------- | ----------- | -------------- |
| 8     | fits        | fits        | fits        | fits           |
| 16    | fits        | fits        | fits        | fits           |
| 32    | fits        | fits        | tight       | wraps (33 cells + spaces) |
| 64    | overflow → text | overflow → text | overflow → text | overflow → text |

A `t` key toggles 7-seg / plain text regardless of fit, in case the
user prefers compact display.

#### Plain-text fallback rendering

DEC, with comma grouping every 3 digits (integer part):
```
4,294,901,760
```

HEX, with `_` every 4 digits, `0x` prefix, uppercase:
```
0xFFFF_0000
```

OCT, with `_` every 3 digits, `0o` prefix:
```
0o37_777_600_000
```

BIN, with `_` every 4 digits, `0b` prefix:
```
0b1111_1111_1111_1111_0000_0000_0000_0000
```

All renderings reflect the current `width`: at width=8, leading bits
above bit 7 are not displayed.  Negative numbers in signed view show
a leading `-`; in unsigned view, the same bit pattern shows as the
two's-complement positive value.

### Input keys

PPAP user space has a real keyboard available on every supported TTY
(serial, PicoCalc), so we type rather than click on virtual buttons.

| Key            | Action                                        |
| -------------- | --------------------------------------------- |
| `0`–`9`        | digit (rejected if invalid for current base)  |
| `a`–`f`        | hex digit (rejected outside HEX)              |
| `+ - * / %`    | arithmetic op                                 |
| `&`            | AND                                           |
| `|`            | OR                                            |
| `^`            | XOR                                           |
| `~`            | NOT (unary, applied to current `display`)     |
| `<`            | shift left (single keypress, not C `<<`)      |
| `>`            | shift right                                   |
| `=` or Enter   | commit pending op                             |
| `.`            | (reserved for future REAL mode)               |
| Backspace      | delete last entered digit                     |
| `c`            | clear entry (CE)                              |
| `C`            | all-clear (AC)                                |
| `n`            | negate (`+/-`)                                |
| `d` `h` `o` `b`| switch base to DEC / HEX / OCT / BIN          |
| `w`            | cycle width: 8 → 16 → 32 → 64 → 8             |
| `s`            | toggle signed / unsigned display              |
| `t`            | toggle 7-seg / plain-text display             |
| `q`            | quit                                          |
| `?`            | show help overlay                             |

Key conflicts to be aware of: `c` vs `C` are case-distinguished, so
both are reachable on a real keyboard.  `b` (BIN switch) cannot be
typed as a hex digit — that's fine, hex digits are `a`–`f` only.

### Display layout

Minimum view (PicoCalc 40×20), 7-seg display, DEC width=32:

```
+--------------------------------------+
| DEC  W=32  signed                    |   status line
|                                      |
|       _   _       _   _   _   _      |
|      |_| | | |_| |_  |_| |_  |_      |   7-seg display, 3 rows
|      |_| |_|   | |_| |_|  _| |_|     |   (right-aligned)
|                                      |
| ans = 65535                          |   last result
+--------------------------------------+
| 1+2= 3   3*4= 12   12&5= 4           |   recent ops (single line)
+--------------------------------------+
| [d/h/o/b] base   [w] width  [s] sign |
| [c] CE  [C] AC   [n] +/-  [t] 7-seg  |
| [q] quit  [?] help                   |
+--------------------------------------+
```

In BIN mode the three 7-seg rows collapse to a single LED-dot row
(`●●●● ●●●● ○○○○ ○○○○`), freeing two rows for tape on the small
view.  When the value cannot fit (e.g. width=64 in any base), the
display falls back to one row of plain text and the freed rows
become tape.

Wide view (≥80 cols, e.g. serial / QEMU / PicoCalc 80×40):
the recent-ops line expands into a multi-line **tape pane** on the
right side, the status line gains base/width/sign as labeled fields,
and the help footer shows the full key table.

```
+----------------------------------+--------------------------+
| DEC  W=32  signed                | TAPE                     |
|                                  |   1                      |
|     _   _       _   _   _   _    | + 2                      |
|    |_| | | |_| |_  |_| |_  |_    | = 3                      |
|    |_| |_|   | |_| |_|  _| |_|   | * 4                      |
|                                  | = 12                     |
| ans = 65535                      | & 5                      |
+----------------------------------+ = 4                      |
| [d] DEC  [h] HEX  [o] OCT  [b] B |                          |
| [w] width   [s] sign   [n] +/-   |                          |
| [c] CE  [C] AC  [t] 7-seg        |                          |
| [q] quit  [?] hl                 |                          |
+----------------------------------+--------------------------+
```

Layout decisions:
- TTY size is queried via `TIOCGWINSZ`; if unsupported, fall back to
  80×24.  The layout function is called on every redraw, so SIGWINCH
  is not strictly required (we redraw on every keypress anyway).
- Three breakpoints: `< 60 cols` → minimum view; `60–79` → minimum
  view with wider value field; `≥ 80` → tape pane on the right.
- The display value is right-aligned in a fixed field; widths wider
  than the field are truncated with a leading `…` (rare for INT —
  64-bit binary is 67 chars with `_` and `0b`, longer than 40 cols).
  At width=64 in BIN on a 40-col TTY the value wraps to two lines;
  this is acceptable.

### File layout

```
src/user/calc.c                main loop, layout, key dispatch, drawing
src/user/calc_state.c          state machine + ops + width masking
src/user/calc_state.h          public types and entry points
src/user/calc_render.c         value → digit string with grouping
src/user/calc_render.h
src/user/calc_segdisp.c        7-seg glyph table + multi-line layout
src/user/calc_segdisp.h        + LED-dot row for BIN
tests/host/test_calc_state.c   key-sequence-driven state-machine tests
tests/host/test_calc_render.c  rendering tests for all bases / widths
tests/host/test_calc_segdisp.c 7-seg layout / fit / fallback tests
```

`calc_state.c`, `calc_render.c`, and `calc_segdisp.c` are pure —
no syscalls, no IO — so they are fully host-testable.  The TTY
layer in `calc.c` is a thin shell over them.  `calc_segdisp.c`
takes a digit string from `calc_render.c` and produces an array of
N output lines (1 for plain text or LED-dot row, 3 for 7-seg).

### Wire-up

- Add `calc` to `USER_APPS` in [cmake/user.cmake](/cmake/user.cmake).
- Install in romfs per the three-list update procedure (the same
  procedure used for every native userland app — see existing
  examples like `pi`, `pile`).
- For pcxt, evaluate kernel-segment size impact; calc is user-space
  only so this should be free, but the romfs grows.
- Smoke test in QEMU: drive `calc` from `push` with a scripted key
  sequence (using single-quoted escape strings — see the quote-
  escapes note in project memory) and grep the output for the
  expected display.

## Phasing

### Phase 1 — integer programmer calc

1. **Engine.**  Implement `calc_state.{c,h}` and `calc_render.{c,h}`
   with host tests.  No UI yet.  This is the bulk of the correctness
   work — width wrapping, base switching, op folding, divide-by-zero,
   sign-toggle rendering.
2. **7-seg renderer.**  Implement `calc_segdisp.{c,h}` with the
   glyph table, multi-line layout for DEC/HEX/OCT, LED-dot row for
   BIN, and fit/fallback logic.  Host tests cover all bases and the
   fallback boundary.
3. **TTY frontend.**  Implement `calc.c` with the minimum-view
   layout (using the 7-seg renderer).  Wire to romfs and verify it
   runs on `qemu_arm`.
4. **Responsive layout.**  Add `TIOCGWINSZ` query, the three
   breakpoints, and the tape pane.  Verify on a wide serial TTY.
5. **PicoCalc check.**  Run on real hardware in 40×20 mode and
   confirm the minimum view fits.  Try color (red/amber lit
   segments) if `fb_con` ANSI color is available.  Adjust if needed.
6. **Install across targets.**  `pcxt`, `xtensa_cc`, `qemu_m68k`,
   `pico1`, `pico2*` — verify build size impact, romfs fit.

### Phase 2 — real numbers in DEC mode (future)

Deferred.  Re-decide between decimal fixed-point and soft-float at
that time.  Constraints to remember:

- `uc_printf` does not support `%f` — a formatter must be written
  either way.
- Bitwise ops on a non-integer value: refuse with a flashed error.
- Mode switch HEX/OCT/BIN with a non-integer DEC value: refuse, do
  not silently truncate.
- If soft-float is chosen, expose the IEEE-754 bit pattern when
  switching to HEX while a REAL value is shown — this is uniquely
  programmer-useful.

### Phase 3 — semi-graphic display (future, separate proposal)

Extend `fb_con` with a per-TTY user-glyph table (e.g. PUA codepoints
`U+E000`–`U+E0FF` mapped to RAM glyph slots) so that apps can
`ioctl` glyphs in and draw with regular `write()`.  This benefits
`calc`, `pi`, `pile`, and any future apps that want bespoke glyphs
without opening a pixel API.  Filed here for context only; the
design and work belong in their own proposal.

## Open Questions

None blocking Phase 1 — the four defaults (real width wrap, tape
pane day-one, no memory registers, no named variables) have been
agreed and are reflected above.  Items deferred to later phases:

- DEC real-number representation (Phase 2).
- Whether to add an expression mode (`:e <expr>`) alongside the
  button-calc UI.
- Custom-glyph mechanism in `fb_con` (Phase 3, separate proposal).
