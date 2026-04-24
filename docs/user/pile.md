# pile — adaptive two-pane filer

## Overview

**pile** is a PPAP-native TUI file manager in the tradition of **mint**
(X68000, SALT) and **Norton Commander** / **Midnight Commander**, with
a built-in text viewer and hex viewer.  A single ELF ships for every
PPAP target — pcxt, ARM, m68k, RISC-V, Xtensa — and adapts its layout
at runtime to whatever `TIOCGWINSZ` reports: two panes when the
terminal is wide enough, single-pane otherwise.

## Launching

```
pile [--no-color] [path]
```

- With no argument, pile opens in the current working directory.
- `--no-color` disables VT100 color (same effect as `NO_COLOR=1` in
  the environment).

To quit: `q` / `Ctrl-Q`.

## Layout

pile picks a layout from the column count at startup.  `Ctrl-L`
re-queries `TIOCGWINSZ` and flips the layout if the width crossed a
threshold.

| Cols | Layout | Example |
|---|---|---|
| ≥ 70 | **two pane** | pcxt 80×25, PicoCalc 80×40, serial 80×24 |
| 40 ≤ cols < 70 | **single pane** | pcxt 40×25, PicoCalc 40×20, narrow serial |
| < 40 | **refuse** | Exits with "pile: terminal too narrow" |

### Two-pane (≥ 70 cols)

```
-- /usr/bin ---------------|-- /tmp ---------------------
 ..                  <DIR> | ..                    <DIR>
 ls                  12024 | foo.txt                 342
*cat                  9264 | bar.bin                1024
 push                27708 |
 pi                  20412 |
                           |
  5/32  2 sel         12K  |  2/8                    1K
---------------------------+---------------------------
  /usr/bin/cat
  9264  -rwxr-xr-x  2026-04-22 21:39
------------------------------------------------------- ?: help
```

- Top area: two side-by-side panes, each with a path header and a
  per-pane footer (`cursor / count  [N sel]  total`).
- Middle strip: the cursor entry's full path plus size / mode / mtime.
- Bottom rule: `?: help` opens a full-screen key reference.
- Active pane is highlighted in cyan; inactive pane uses plain
  reverse video for its cursor so it's visible but clearly secondary.

### Single pane (40 ≤ cols < 70)

The same sections stacked.  `TAB` swaps which pane is currently
displayed; the hidden pane keeps its cursor and path so it can still
serve as the copy / move destination.

### Viewer overlay

Both viewers take over the full screen.  `q` / `ESC` returns to the
filer exactly where it was.

## Key bindings

Press `?` in pile for a live reference.

### Navigation

| Key | Action |
|---|---|
| `↑` / `↓` | Move cursor |
| `PgUp` / `PgDn` | Page up/down |
| `Home` / `End` | Top / bottom of listing |
| `TAB` | Switch active pane |
| `←` / `→` | Spatial: switch to the other pane, or go to parent from the side facing away |
| `ENTER` | Enter directory, or view the file |
| `BS` | Parent directory |

### Marking

`pile` marks operate on the active pane.  File operations (`c` / `m` /
`d`) act on the marked set if any entries are marked, otherwise on
the entry under the cursor.

| Key | Action |
|---|---|
| `SPACE` | Toggle mark on cursor, step down |
| `+` | Mark by glob (prompt) |
| `-` | Unmark by glob (prompt) |
| `*` | Invert all marks |

Glob syntax is shell-style — `*` matches any run, `?` matches any
single character.  Both ends of the name are anchored.

### Actions

| Key | Action |
|---|---|
| `v` | View — auto-detect text or hex |
| `V` | View — force hex |
| `e` | Edit (spawns `/bin/pi`) |
| `c` | Copy marked set to the other pane's directory |
| `m` | Move / rename marked set to the other pane (same-dir move prompts for a new name) |
| `d` | Delete marked set (single confirmation) |
| `k` | mkdir (prompt) |
| `s` | Cycle sort: name → size → mtime |
| `.` | Toggle show-hidden (dotfiles) |
| `!` | Spawn `/bin/sh` in the active pane's directory |
| `Ctrl-L` | Redraw / re-query terminal size |
| `q` / `Ctrl-Q` | Quit |

Prompts (copy / move destination, rename, mkdir, glob, hex offset)
use the simple in-line editor at the bottom row: `ENTER` accepts,
`ESC` cancels, `BS` deletes.

## Viewer

The viewer is streaming — files are read on demand from disk, so
there is no per-file size limit and pile's data segment stays small.

### Auto-detection

On `v`, pile sniffs the first 2 KB of the file.  If ≥ 95 % of those
bytes are printable ASCII plus `\t` / `\n` / `\r`, it lands in the
text viewer; otherwise the hex viewer.  Empty files are treated as
text.

SJIS / UTF-8 files will fail the ASCII check and open in hex.  Use
`V` to force hex explicitly.

### Text viewer

```
-- file.c  line 1/142 ------------------------------------
#include "pile.h"
...
---------------------------------------------------------
 PgUp/PgDn page  g/G start/end  V hex  q quit
```

- Hard line wrap is not supported; lines longer than the viewport
  are truncated at the right edge.
- Tabs expand to 8-column stops.
- Non-printable bytes render as `.`.
- Navigation: `↑` / `↓` / `k` / `j` per line, `PgUp` / `PgDn` per
  page, `g` / `G` to top / bottom.

### Hex viewer

```
-- file.bin  offset 0000 / 2430 --------------------------
0000  7f 45 4c 46 01 01 01 00  00 00 00 00 00 00 00 00  |.ELF............|
0010  02 00 28 00 01 00 00 00  e9 01 01 00 34 00 00 00  |..(.........4...|
...
---------------------------------------------------------
 PgUp/PgDn page  g/G start/end  : goto  q quit
```

- 16 bytes per row on ≥ 74 cols, 8 bytes per row otherwise.
- The offset column auto-widens for the file size (4, 6, or 8 hex
  digits).
- `:` prompts for a hex offset.  Both `1000` and `0x1000` are
  accepted; the offset is rounded down to a row boundary.

## Color

pile emits VT100 color by default.

| Role | Color | Used for |
|---|---|---|
| Directory | bold blue | dir entries, path header |
| Executable | bold green | files with any `x` bit |
| Symlink | cyan | symbolic links |
| Device | bold yellow | character / block devices under `/dev` |
| Active | bold white on cyan | active pane header and cursor |
| Inactive cursor | reverse video | inactive pane's cursor |
| Mark | reverse video + leading `*` in the gutter | marked entries |

Non-color fallbacks: directories get a trailing `/`, executables
`*`, symlinks `@`; the cursor always uses reverse video.  With
`--no-color` pile still renders correctly on a monochrome serial
terminal.

## Limitations

Intentional scope choices for the current release:

- **No archives.**  `pile` runs the files it finds and does not
  decompress.
- **No built-in editor.**  `e` spawns `/bin/pi`.
- **No recursive copy / delete.**  Marking a directory will refuse
  on batch `c` / `d` until `uc_fts` lands in uclib.
- **No search in the viewer.**  Use `push | grep` externally, or
  the hex viewer's `:offset` jump if you already know the location.
- **128 entries per pane.**  Directories beyond that show a `+` in
  the pane footer; marking-by-glob still matches across the full
  directory, but navigation is capped at the first 128 entries.
- **Dotfiles hidden by default.**  Press `.` to reveal them; `..`
  always stays visible regardless.

## See also

- [docs/user/push.md](push.md) — PiPAPo μShell; `!` from pile
  spawns a push session.
- [docs/user/uc_malloc.md](uc_malloc.md) — the userland heap that
  backs pile's per-op scratch buffers.
