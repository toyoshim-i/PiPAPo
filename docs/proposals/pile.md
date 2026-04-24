# Proposal: `pile` — adaptive two-pane filer

## Summary

`pile` is a PPAP-native TUI file manager in the tradition of
**mint** (X68000, SALT) and **Norton Commander** / **Midnight
Commander**, with a built-in text viewer and hex viewer.  It is a
single bare-metal ELF that runs on every PPAP target, adapting its
layout at runtime to whatever `TIOCGWINSZ` reports: two panes when
the terminal is wide enough, single-pane (FD-style) otherwise.

The primary target is **pcxt** — where a one-screen filer is the
most useful interactive tool after `push` itself — but the same
binary ships for ARM, m68k, RISC-V and Xtensa via the standard
Path A toolchain (`src/user/` + `uclib`).

Normative references:

- [docs/user/userland_dev_guide.md](/docs/user/userland_dev_guide.md)
  — compiler flags, linking, memory layout, ELF loader contract.
- [docs/proposals/more_userland_apps.md](/docs/proposals/more_userland_apps.md)
  — the broader native-userland replacement programme; `pile`
  slots in as a "Tier N" net-new applet, not a busybox shadow.
- [src/user/README.md](/src/user/README.md) — directory layout and
  "Adding a New Program" checklist.
- [docs/getting_started/coding_rules.md](/docs/getting_started/coding_rules.md)
  — style rules.

## Motivation

- **pcxt needs a filer.**  pcxt boots straight into `push`, and
  even trivial directory browsing — "what's on the UFS HDD image,
  what do I want to run" — currently means `ls` / `cd` / `cat`
  loops.  Every DOS-era PPAP contemporary (FD, FILMTN, mint, NC)
  had a filer as the default interactive tool for a reason.
- **Built-in viewers avoid process juggling.**  With 8-process
  system-wide cap and 128 KB per-process data budget, spawning a
  separate `cat`/`less` for every "what's in this file?" glance
  is wasteful.  An integrated viewer stays inside the filer
  process and flips modes with one keystroke.
- **Same binary everywhere.**  `pile` uses `TIOCGWINSZ` + the
  existing uclib I/O stubs; no arch-specific code.  The 80×25
  pcxt console, 80×40 PicoCalc LCD, 40×20 PicoCalc, and 80×24
  host serial all render from the same code path.
- **Mirrors the push / pi aesthetic.**  PPAP already ships a
  screen-editor (`pi`) and a shell line-editor (`push_line`).  A
  filer fills out the triangle of interactive tools, sharing
  idioms (VT100 escapes, `uc_*` helpers, single-file-per-applet
  where possible).

## Non-Goals

- **Archiver / plugin architecture (for now).**  mint's strength
  was its LZH/ZIP/plugin ecosystem.  A plugin host would blow
  both the binary size budget and the 128 KB data budget today,
  and PPAP has no LHA/ZIP userland either.  `pile` v1 runs the
  binaries it finds and does not decompress anything.

  **Longer-term direction.**  Extensibility is a goal, just not
  a v1 goal: once the core filer is stable we expect to embed a
  small lisp-like interpreter (think "tinylisp"-scale, evaluator
  + a handful of builtins, no GC sophistication) as the plugin
  surface.  User-written expressions would define file-type
  handlers (what to run on `ENTER` for a `.x` file), custom
  viewers, and batch operations over marked files.  This
  proposal deliberately does **not** bake any plumbing for it
  in — adding config hooks before the interpreter exists would
  be speculative infrastructure — but the architectural
  boundaries noted below (viewer dispatch table, file-op
  registry) are shaped so a later interpreter can plug in
  without restructuring.
- **Built-in editor.**  Viewing is read-only.  If the user
  presses an "edit" key we spawn `pi`; we do not duplicate its
  buffer logic.
- **POSIX `mc` compatibility.**  Key bindings borrow from mint /
  NC / mc but do not try to match any of them byte-for-byte.
- **Remote filesystems / VFS plugins.**  `pile` sees only what
  the PPAP VFS already mounts.  No FTP, no tar-as-directory.
- **Localisation.**  ASCII + VT100.  Any SJIS / UTF-8 view work
  is deferred to a future iteration (the hex viewer is the
  escape hatch until then).
- **Become a busybox shadow.**  There is no `CONFIG_MC` to drop;
  this is a net-new applet, not part of the tier-by-tier
  busybox shrink plan.

## Layout adaptation

`pile` queries `ioctl(0, TIOCGWINSZ, &ws)` at startup and on
every `SIGWINCH` (if we wire one up later).  It picks one of
three layout classes from the column count; row count only
affects how many entries fit per pane.

| Cols | Layout | Notes |
|---|---|---|
| ≥ 70 | **two pane** | Default on pcxt 80×25, PicoCalc 80×40, serial 80×24. |
| 40 ≤ cols < 70 | **single pane** | pcxt 40×25, PicoCalc 40×20, narrow serial. |
| < 40 | **refuse** | Print "pile: terminal too narrow (need ≥40 cols)\n" and exit 1. |

### Two-pane (≥70 cols)

```
┌── /usr/bin ─────────────────┬── /tmp ────────────────────┐
│ ..                    <DIR>│ ..                    <DIR>│
│ ls                    12024│ foo.txt                 342│
│*cat                    9264│ bar.bin                1024│
│ push                  27708│                            │
│ pi                    20412│                            │
│                            │                            │
│ 5 / 32  2 sel           12K│ 2 / 8    0 sel          1K │
├────────────────────────────┴────────────────────────────┤
│ /usr/bin/cat                                            │
│ 9,264 bytes  -rwxr-xr-x  2026-04-22 21:39               │
├─────────────────────────────────────────────────────────┤
│F1 help  F3 view  F5 copy  F6 move  F7 mkdir  F8 delete  │
│F10 quit  TAB switch  SPACE mark  ENTER open             │
└─────────────────────────────────────────────────────────┘
```

- Top third of the screen is two side-by-side panes, each with a
  path header and a per-pane footer (count / selection / total
  size).
- Middle strip shows the cursor file's full path and stat info.
- Bottom strip is the key legend.  When key count exceeds width
  we drop legend columns (leaving the essentials: F5 / F8 / F10).

### Single pane (40 ≤ cols < 70)

```
┌── /usr/bin ──────────────────────────┐
│ ..                            <DIR> │
│ ls                            12024 │
│*cat                            9264 │
│ push                          27708 │
│ pi                            20412 │
│ ...                                 │
│ 5 / 32  2 sel  total 12K            │
├──────────────────────────────────────┤
│ /usr/bin/cat                         │
│ 9264  -rwxr-xr-x  2026-04-22 21:39   │
├──────────────────────────────────────┤
│F3 view  F5 copy  F8 del  F10 quit   │
└──────────────────────────────────────┘
```

- Same sections stacked; "other pane" still exists conceptually
  and holds the copy/move destination, but is not drawn.  `TAB`
  swaps the visible pane with the hidden one.
- Key legend shows only the most common operations.

### Viewer overlays

Both text and hex viewers take over the full screen (no panes)
while active.  `q` / `ESC` / `F10` returns to the filer exactly
where it was.

**Text viewer.**

```
┌── /etc/passwd ──────── 1/3 pages ──── 42 lines ────────┐
│ root::0:0:root:/root:/bin/sh                           │
│ ...                                                    │
├────────────────────────────────────────────────────────┤
│PgUp/PgDn  g/G top/bot  /  search   q quit              │
└────────────────────────────────────────────────────────┘
```

- Line-oriented.  Line wrap on by default (wrap indicator
  `+` in the gutter); `w` toggles off and switches to horizontal
  scroll with `h` / `l`.
- No tabs expansion beyond replacing `\t` with 8 spaces and
  translating non-printable 0x00–0x1F to `·` (U+00B7 fallback:
  ASCII `.`).

**Hex viewer.**

```
┌── /usr/bin/cat ────────── offset 0x0000 / 0x2430 ──────┐
│00000000  7f 45 4c 46 01 01 01 00  00 00 00 00 00 00 00 00  |.ELF............|
│00000010  02 00 28 00 01 00 00 00  e9 01 01 00 34 00 00 00  |..(.........4...|
│ ...                                                       │
├────────────────────────────────────────────────────────┤
│PgUp/PgDn  g/G  :offset  q quit                         │
└────────────────────────────────────────────────────────┘
```

- 16 bytes per line on ≥70 cols, 8 bytes per line on narrow.
- Offset display auto-widens to accommodate the file size.
- `:` prompts for a hex offset (`:1000`, `:0x1000`, both work)
  and jumps.

## Output style: colorful VT100

Following the PPAP native-userland convention documented in
[more_userland_apps.md §Output style](/docs/proposals/more_userland_apps.md#output-style-colorful-vt100-by-default)
(see `ls`, `ps`, `top`, `push_line` for precedent), `pile` emits
VT100 color by default, with `--no-color` / `NO_COLOR` turning it
off.  The palette is intentionally small so each sequence stays
flash-resident (ARM XIP) via the `C(...)` indirection:

| Role | Color | Used for |
|---|---|---|
| `C_DIR` | bold blue | Directory entries (both panes and stat strip) |
| `C_EXEC` | bold green | Executable files (mode bits or known extensions) |
| `C_LINK` | cyan | Symlinks |
| `C_DEV` | yellow | Device nodes under `/dev` |
| `C_MARK` | reverse video | Marked entries (overrides type color) |
| `C_CUR` | bold + reverse | Cursor line (active pane); reverse only on inactive pane |
| `C_FRAME` | dim | Box-drawing characters, separators |
| `C_HEADER` | bold | Pane path header, viewer title |
| `C_KEY` | bold | Function-key names in the legend strip (`F3`, `F5`, …) |
| `C_WARN` | yellow | Transient warnings under the stat strip |
| `C_ERR` | red | Error messages |

Viewer colorisation:

- **Text viewer.**  Non-printable bytes (the `·` substitutes) in
  `C_WARN` so the user can tell them apart from real dots.  The
  search match (when `/` is used) highlights in reverse video.
- **Hex viewer.**  Offset column in dim, hex bytes default, ASCII
  gutter dim; the byte at the current cursor (`:offset` target
  or search hit) in `C_CUR`.  Zero bytes optionally in dim to
  make structure pop — decide during P4 implementation by eye.

Rules per the convention:

- A single `C_*` block at the top of `pile.c` (or `pile.h` if
  shared across files) with only the sequences pile actually
  uses.  No shared palette header.
- `--no-color` and `$NO_COLOR` both set `use_color = 0`; no
  other control knobs.
- Every color channel has a non-color fallback: directories
  append `/` (like `ls`), executables `*`, marks are drawn with
  a leading `*` glyph in the name column regardless of color,
  the cursor uses reverse video which works even without SGR
  color.  Color is decoration; the layout still reads correctly
  on a monochrome serial console.

## Key bindings

Two overlapping sets; both are always active so users on
keyboards without function keys (serial consoles, some
PicoCalc layouts) can still drive the app.

### Global

| Key | Action |
|---|---|
| `F10` / `q` / `Ctrl-Q` | Quit |
| `F1` / `?` | Help overlay |
| `TAB` | Switch active pane (two-pane) / swap visible pane (single-pane) |
| arrows / `hjkl` | Move cursor |
| `PgUp` / `PgDn` | Page up/down |
| `Home` / `End` / `g` / `G` | Top / bottom of listing |
| `ENTER` | Enter directory / run executable / view file by type |
| `BS` | Parent directory (same as `cd ..`) |
| `SPACE` | Toggle mark on cursor file, move down |
| `+` / `-` | Mark-by-pattern / unmark-by-pattern (prompts) |
| `*` | Invert marks |
| `Ctrl-R` | Refresh both panes |

### File operations (legend-grade)

| Key | Action |
|---|---|
| `F3` / `v` | View (text or hex by default → text if file looks ASCII, else hex) |
| `Shift-F3` / `V` | View forced as hex |
| `F4` / `e` | Edit (spawns `/bin/pi`) |
| `F5` / `c` | Copy marked (or cursor) to other pane's dir |
| `F6` / `m` | Move / rename.  Same-dir move prompts for new name. |
| `F7` | mkdir (prompt) |
| `F8` / `d` / `DEL` | Delete marked (or cursor).  Prompts once for the whole batch. |
| `F9` | Menu (sort / filter / about) |
| `!` | Run shell command in cwd of active pane |

All operations use the push line editor (`push_line`) for prompts
— same history, same editing keys.  This is mechanical code
reuse, not a new UI.

### Sort / filter

`F9` opens a vertical menu (arrow + enter).  Options:

- Sort by: name / size / mtime / extension
- Sort direction: asc / desc
- Show dotfiles: on / off
- Filter glob: `*` / custom

All choices are session-only (no config file).  The push
environment variable `PILE` is consulted at startup for initial
sort (`PILE=sort=mtime,desc`); nothing else is persisted.

## Source layout

```
src/user/pile/
  pile.c           entry + main loop + global state
  pile_pane.c      pane model: read dir, sort, scroll, mark
  pile_draw.c      VT100 rendering: two-pane / single-pane / status / keys
  pile_ops.c       file operations: copy / move / delete / mkdir / rename
  pile_view.c      text viewer + hex viewer (shared scroll/search)
  pile.h           shared types
```

One folder under `src/user/`, following the `pi/` and `pdb/`
pattern.  `cmake/user.cmake` gains a `PPAP_USER_MAIN_SOURCE_pile`
+ `PPAP_USER_EXTRA_SOURCES_pile` block identical in shape to the
existing `pi` block.

Per-target install (**three lists, see
[pcxt user install reference](../../.claude/projects/-home-toyoshim-Work-PPAP/memory/reference_pcxt_user_install.md)**):

1. `USER_APPS` in [cmake/user.cmake](/cmake/user.cmake) — append `pile`.
2. `PCXT_USER_APPS` in
   [src/target/pcxt/CMakeLists.txt](/src/target/pcxt/CMakeLists.txt)
   — append `pile`.
3. `USER_APPS=(...)` in
   [scripts/mkpcimg.sh](/scripts/mkpcimg.sh) — append `pile`.

## Memory budget

Per-process, at the 128 KB data limit:

| Item | Size | Notes |
|---|---|---|
| Two pane entries × 256 slots × ~64 B | ~32 KB | Name + stat fields |
| Line buffer for text viewer | 4 KB | 1 page; reused for hex viewer |
| Viewer read buffer | 4 KB | Shared with line buffer when hex-only |
| Marking bitmap (256 bits × 2 panes) | 64 B | Bit-per-entry |
| Scratch for prompts / tempnames | ~2 KB | Path + name buffers |
| Draw ring / dirty tracking | ~4 KB | Double-buffer one line per column |
| **Total dynamic** | **~50 KB** | Leaves headroom for stack / GOT / BSS |

Pane cap: **256 entries per pane**.  Larger directories display a
`… N entries truncated` tail line; marking-by-pattern still walks
the full directory via `readdir`.

Binary size guidance (measured against [more_userland_apps.md §Size budget](/docs/proposals/more_userland_apps.md#size-budget)):
the hard limit on pcxt is the i8086 64 KB segment (text + rodata
+ data + bss + stack), with the crt0 and uclib tax applied on top.
A multi-file interactive app with viewer logic should land in
the `pi` (20 KB) to `pdb` (33 KB) range on pcxt; anything under
40 KB has comfortable headroom.  No explicit soft target — watch
the per-phase growth and reach for cuts only if we approach the
segment limit.

## Phased rollout

Each phase lands as its own commit / review, matching the project
discipline of "one functional increment per commit" (see
[`feedback_commit_scope.md`](../../.claude/projects/-home-toyoshim-Work-PPAP/memory/feedback_commit_scope.md)).

### Phase P1 — Skeleton + single pane listing

- `src/user/pile/` created with the four files that carry P1
  logic: `pile.h`, `pile.c` (main loop + raw termios + key
  reader), `pile_pane.c` (dir read / sort / cursor), and
  `pile_draw.c` (single-pane renderer).  `pile_ops.c` and
  `pile_view.c` are added in P3 and P4 when they have real
  code — empty stubs are not committed up front.
- `pile_pane.c` reads the cwd, sorts by name (directories
  first, `..` pinned to top), handles cursor + PgUp/PgDn +
  `ENTER` on directories.  No file action yet.
- `pile_draw.c` renders the single-pane layout, shaped so the
  P2 two-pane expansion just adds a second `pile_pane_t` and
  calls the same helpers with a column offset.
- `F10` / `q` / Ctrl-Q exits cleanly (restoring termios,
  clearing screen, showing the cursor).
- Installed on all three install paths (cmake USER_APPS,
  PCXT_USER_APPS, mkpcimg.sh USER_APPS).
- Smoke test: `./scripts/run.sh --build pcxt`, launch `pile`,
  navigate `/usr/bin`, `ENTER` into a dir, `BS` back up, quit.

### Phase P2 — Two-pane layout + adaptive detection

- `TIOCGWINSZ` probe + layout selection (≥70 two-pane, else
  single).
- Second pane, `TAB` switches active.
- Stat strip + key legend strip.
- Mark with `SPACE`; mark count in pane footer.

Verification: resize a host terminal (QEMU `-nographic` + tmux)
below/above 70 cols; pile redraws correctly on the next
keypress (full SIGWINCH wiring is Phase P5).

### Phase P3 — File operations

- `F5` copy (single file first; marked batch second commit if
  the single-file path lands cleanly).
- `F6` move (rename fast-path when cross-pane stays on same FS;
  copy+unlink fallback otherwise — same pattern `mv` uses today).
- `F7` mkdir, `F8` delete (with single batch confirmation).
- Prompts go through `push_line`.  Error reporting is one-line
  under the stat strip; errors auto-clear on the next keypress.

### Phase P4 — Viewers

- Text viewer: line wrap / scroll / `g G /`.
- Hex viewer: 16 or 8 bytes per row, `:offset` jump.
- Auto-detect: sniff first 4 KB for > 95 % printable → text,
  else hex.  `Shift-F3` forces hex.
- Both viewers share the file-read + keyboard loop.

### Phase P5 — Polish

- `SIGWINCH` handler: redraw on resize.
- `F9` menu for sort / filter / dotfiles.
- `!` shell spawn (vforks `/bin/push` with `-c`, waits).
- `F4` spawn `/bin/pi`; if absent, error one-line.
- Help overlay (`F1` / `?`).

Phases P1–P4 must land.  P5 is quality-of-life — skip entries
can be dropped if size gets tight.

### Deletion

After P5 lands (or after the proposal author decides P5 is
not going to ship), follow the
[`reference_proposal_lifecycle.md`](../../.claude/projects/-home-toyoshim-Work-PPAP/memory/reference_proposal_lifecycle.md)
rule: `git rm docs/proposals/pile.md`, drop any tree-wide
references, commit together.

## Open Questions

- **Small-terminal threshold.**  Is 70 cols the right cutoff
  for two-pane?  An 80-col terminal with the stat / key strips
  feels right; 60 is too cramped.  Validate by eye during P2.
- **Which key set wins in documentation?**  Function-key
  legend is the "approachable" default; letter-keys are the
  power-user path.  Ship docs mentioning both, default legend
  shows F-keys, but any help / README example uses letters
  first.  Revisit if user feedback leans one way.
- **Text viewer's ASCII check on SJIS files.**  Human68k / DOS
  text files are SJIS.  The heuristic above will classify most
  as "binary" because of the 0x80–0xFF octets.  Acceptable for
  v1: user presses `F3` then `Shift-F3`, and hex is a sensible
  fallback.  Native SJIS rendering would be its own proposal
  after an SJIS → VT-safe decoder exists in uclib.
- **Interpreter shape, when it lands.**  The longer-term
  lisp-ish plugin path (see Non-Goals) raises decisions we
  don't owe answers to yet but should not paint ourselves out
  of: single-file `.pile` startup script vs directory of
  handlers, expression language vs forms, whether
  handlers can take over the screen or only emit
  post-processing commands.  Revisit once the core is stable
  enough to know what users actually want to extend.
- **Dir-recursive copy / delete.**  `pile`'s `F5` / `F8` on a
  directory today needs a `uc_fts`-style helper that
  [more_userland_apps.md Tier 2](/docs/proposals/more_userland_apps.md#tier-2--file-operations-landed)
  explicitly deferred.  First cut: refuse with "pile: use
  rm -r / cp -r for directories" if the marked set contains a
  dir.  Revisit once `uc_fts` lands.
- **Mount-boundary handling.**  Copying across mount points
  works file-by-file; moving will hit the cross-device case we
  already handle in `mv` (copy + unlink).  Nothing pile-specific
  here but worth flagging in the P3 test plan.

## Verification

- `./scripts/run.sh --build pcxt` — launch, navigate, view, copy,
  delete in the interactive session.
- `./scripts/run.sh --build pico1calc` — same on 80×40 LCD and
  40×20 LCD (`ttyctl` to switch modes); verify layout flips.
- `./scripts/run.sh --test qemu_arm` + `qemu_m68k` — build-only
  verification that the shared sources compile for every arch.
- Size regression: `ls -l build/pcxt/user/pile.elf` after every
  phase; bail (or cut scope) if it exceeds 25 KB.
