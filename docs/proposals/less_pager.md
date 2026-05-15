# `less` Pager (with `more` as a symlink)

**Status:** proposed.  PPAP currently ships no pager — `cat` is the only way
to dump a file to the console.  This proposal adds a single binary, `less`,
that doubles as `more` via an `argv[0]` switch, and follows the same
colorful VT100 visual direction already used by `top`, `ps`, `ls`, `pile`,
and `pi`.

## Goal

Provide an interactive pager that:

- Fits within the per-process budget (128 KB data on i16, 256 KB elsewhere;
  4 KB stack).  See [userland_dev_guide.md §1](../user/userland_dev_guide.md#1-platform-overview).
- Pages forward and backward through files of arbitrary size without loading
  the whole file into memory.
- Shares one binary between `less` and `more` (symlink in romfs) — `more`
  selects the simpler forward-only behavior at startup based on `argv[0]`.
- Matches the project's existing VT100/ANSI styling vocabulary.

Non-goals: regex search, marks, multi-file `:n`, shell-out, tags, `~/.lesskey`.

## Scope split: `less` vs `more`

The binary inspects `argv[0]` (or `basename(argv[0])`) once at start-up:

| Mode  | Key set                                       | EOF behavior       | Status line                                   |
|-------|-----------------------------------------------|--------------------|-----------------------------------------------|
| `more`| Space/Enter forward, `q` quit                 | Auto-exit at EOF   | Minimal: `--More-- (NN%)`                     |
| `less`| Full key map below                            | Stay until `q`     | Full status: filename · line · % · search hit |

Sharing the binary keeps romfs small (`less` is the only ELF; `more` is one
symlink entry).

## Key bindings (`less` mode)

Match the muscle memory of real `less` for the basics; drop everything else.

| Key                    | Action                                |
|------------------------|---------------------------------------|
| `Space`, `f`, `PgDn`   | Forward one screen                    |
| `b`, `PgUp`            | Backward one screen                   |
| `Enter`, `j`, `Down`   | Forward one line                      |
| `k`, `Up`              | Backward one line                     |
| `g`                    | Go to first line                      |
| `G`                    | Go to last line                       |
| `/pat`                 | Search forward (literal, no regex)    |
| `?pat`                 | Search backward                       |
| `n` / `N`              | Repeat last search forward / backward |
| `-N`                   | Toggle line numbers                   |
| `-S`                   | Toggle chop-long-lines vs wrap        |
| `h`                    | Full-screen help; any key returns     |
| `q`, `Ctrl-C`          | Quit                                  |

Arrow keys arrive as `ESC [ A/B/C/D` — we already parse that pattern in
`pile` and `pi`, so reuse the same decoder shape.

## Memory and file model

The per-process limits forbid `read()`ing the whole file into RAM.  Design:

- Open the file once with `open(O_RDONLY)`; keep the fd for the session.
- Maintain a **line-offset index** — a growable `uint32_t[]` of byte offsets
  for line starts, lazily extended as the user scrolls forward.  Each entry
  costs 4 bytes; a 1 MB log with 16 K lines costs 64 KB — over budget for
  i16 (which targets PC/XT text files), so on i16 the index is capped and
  `G`/`?` fall back to a slow rescan from the top.
- Viewport rendering: `pread()`-style — `lseek()` to the line's offset and
  `read()` enough bytes to fill `nrows × ncols`, then word-wrap or truncate.
- A small ring of recently rendered lines is kept (~32 lines × 256 B = 8 KB)
  so re-rendering after a key press doesn't re-seek for the same view.

Stdin pager mode (`cmd | less`) is a stretch goal — it needs a tmpfs
spool file because we can't seek a pipe.  Out of scope for v1; print
`less: stdin not seekable` and exit until v2.

## Visual direction

Reuse the per-file `C_*` macros from [top.c:19-35](../../src/user/top.c#L19-L35).
Concrete plan:

| UI element        | Style                                                          |
|-------------------|----------------------------------------------------------------|
| File body         | Default (no styling) — we don't repaint terminal-emitted bytes |
| Line numbers (if on) | `C_DIM` gutter, right-aligned, width matches max line digits |
| Status line       | `C_REV` reverse-video bar at last row                          |
| Filename in status| `C_BCYAN`                                                      |
| Position counter  | `C_BYELLOW` (matches `top` numeric accents)                    |
| Search prompt     | `C_REV` + `/` or `?` in `C_BGREEN`                             |
| Search hit        | `C_REV` on the matched substring within the visible viewport   |
| `--More--` (more) | `C_REV` `C_BYELLOW`                                            |
| Help overlay      | Cyan key column + dim description column                       |

The status line always lives on row `nrows-1`; `\033[K` clears it on every
redraw.  We never use 256-color or truecolor escapes — VGA text mode
(PC/XT target) only knows the 16 ANSI colors.

## File layout

```
src/user/less.c                # the pager (one TU, ~600 LoC budget)
docs/user/less.md              # short user-facing manual (key map + caveats)
```

Build wiring:

1. Add `less` to `USER_APPS` in [cmake/user.cmake:456](../../cmake/user.cmake#L456).
2. Add the symlink in [cmake/stage_romfs.cmake](../../cmake/stage_romfs.cmake)
   next to the existing `sh → push` entry:

   ```cmake
   file(CREATE_LINK "less" "${STAGING}/bin/more" SYMBOLIC)
   ```

   `mkromfs` already preserves real symlinks via `lstat`/`S_ISLNK`
   ([tools/mkromfs/mkromfs.c:168](../../tools/mkromfs/mkromfs.c#L168)), so
   nothing else changes.

## Implementation phases

1. **L-1 — skeleton & raw mode.**  TTY raw mode (copy `pile.c` pattern),
   `TIOCGWINSZ`, SIGWINCH redraw, `argv[0]` mode detect, quit on `q`.
2. **L-2 — forward paging.**  Read-on-demand line index, Space / Enter / j,
   status line (filename · line · %).  At this point `more` is functionally
   complete; mark it shippable.
3. **L-3 — backward paging.**  `b`, `k`, `g`, `G`, PgUp/PgDn arrow decoder.
4. **L-4 — search.**  `/pat`, `?pat`, `n`, `N`, hit highlight within
   viewport.  Literal substring match — no regex.
5. **L-5 — polish.**  `-N` line-number toggle, `h` help overlay, color
   pass against `top`/`pile` for consistency.

Commit per phase, scope prefix `less:` (per
[feedback_commit_scope.md](../../.claude/projects/-home-toyoshim-Work-PPAP/memory/feedback_commit_scope.md)).

## Test plan

- `qemu_arm`: pipe a known file (`/etc/profile`, then a synthetic 5000-line
  fixture under `/tmp`) and exercise each binding interactively via the
  QEMU smoke harness — same shape as the `push` smoke tests.  Mind the
  `\X`-in-double-quote and 15 s stdin-warmup gotchas
  ([reference_push_quote_escapes](../../.claude/projects/-home-toyoshim-Work-PPAP/memory/reference_push_quote_escapes.md),
  [reference_qemu_stdin_timing](../../.claude/projects/-home-toyoshim-Work-PPAP/memory/reference_qemu_stdin_timing.md)).
- `qemu_m68k`: same fixture, verify both `less` and `more /etc/motd` work.
- `pcxt`: smaller fixture (≤ 200 lines) — index cap path.
- Build-time: confirm `/bin/more` resolves to `less` via `ls -l /bin` and
  that running `more /etc/motd` enters auto-exit-at-EOF mode.

## Environment variables

- **`TABS`** — tab expansion width.  Parsed as a single decimal integer
  (1–16); falls back to 8 if unset or unparsable.  Read once at startup.
- **`LESS`** — startup flag string, applied before `argv` flags so the
  command line wins.  v1 accepts:
  - `-N` — start with line numbers on
  - `-S` — chop long lines instead of wrapping (default = wrap)
  - `-M` — long status line (default = short)

  Unknown flags are silently ignored — no whining to stderr at startup.

These are the first PPAP user-space tools to read env vars; the parser
lives next to `main()`, no shared lib code.

## Help overlay

`h` clears the screen and renders a full-screen help page (two columns:
cyan key column, dim description column), then waits for any key to
restore the previous viewport from the line ring.  Simpler than a
bottom-half pop-up and matches the redraw model the rest of the pager
already uses.
