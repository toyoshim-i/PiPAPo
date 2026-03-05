# Porting Rogue 5.4.4 to PPAP

This document describes the plan for porting the classic BSD Rogue game
(version 5.4.4) to PicoPiAndPortable.

## Source

- Repository: https://github.com/Davidslv/rogue (BSD license)
- Import as a git submodule under `third_party/rogue`

## Overview

Rogue is a terminal-based dungeon crawler that uses the curses library for
screen drawing. The port involves:

1. Adding the rogue repo under `third_party/`
2. Building a minimal curses shim (pdcurses-nano or hand-written)
3. Cross-compiling rogue against musl + the curses shim
4. Installing the ELF into romfs
5. Kernel-side fixups (if any)

The game renders on a 24×80 terminal — exactly what PPAP's TTY advertises
via `TIOCGWINSZ`. ANSI escape sequences emitted by curses pass through the
PPAP UART transparently; the host terminal emulator (minicom, screen, etc.)
interprets them.

---

## Feasibility Assessment

### What already works

| Requirement          | PPAP status | Notes |
|----------------------|-------------|-------|
| 24×80 terminal       | ✓ | `TIOCGWINSZ` returns 24×80 |
| Raw-mode input       | ✓ | `TCSETS` with `~ICANON ~ECHO` |
| `read`/`write`       | ✓ | syscalls 3/4 |
| `ioctl` (termios)    | ✓ | TCGETS/TCSETS/TIOCGWINSZ |
| `brk` / `malloc`     | ✓ | up to 128 KB heap via musl |
| `time()` / `getpid()`| ✓ | for RNG seeding |
| Signal handling       | ✓ | SIGINT (Ctrl-C) |
| musl libc            | ✓ | printf, stdio, string, stdlib |

### What is missing

| Requirement          | Status | Solution |
|----------------------|--------|----------|
| curses library       | ✗ | Build a **tiny curses shim** (see Step 2) |
| Score file I/O       | partial | tmpfs (8 KB) or disable; SD on PicoCalc |
| `getenv("TERM")`     | partial | musl `getenv` works; set TERM in `/etc/profile` |
| Save/restore game    | partial | Needs writable FS; disable initially |

### Memory budget

Rogue 5.4.4 with PPAP curses shim (measured, xcrypt.c excluded):

| Component       | Size |
|-----------------|------|
| .text (code)    | 143 KB (flash — no limit) |
| .data           | 14 KB |
| .bss            | 61 KB |
| **data+bss**    | **75 KB** |
| Stripped ELF    | 162 KB |

75 KB data+bss is well within the 128 KB per-process limit.
Excluding `xcrypt.c` (71 KB BSS for DES tables) is critical — it's only
used for wizard mode (`MASTER`), which is disabled.

---

## Steps

### Step 0: Import rogue source

```sh
cd third_party
git submodule add https://github.com/Davidslv/rogue.git rogue
```

This places the upstream source in `third_party/rogue/`.

### Step 1: Audit rogue source for portability issues

**Status: COMPLETE** — findings below.

#### 1a. Curses calls used

Rogue uses curses pervasively (included in all 30+ `.c` files via `rogue.h`).
The complete set of curses functions called:

**Screen drawing:**
`initscr`, `endwin`, `clear`, `refresh`, `move`, `addch`, `addstr`,
`mvaddch`, `mvaddstr`, `printw`, `mvprintw`, `clrtoeol`

**Window operations** (rogue uses a scratch `WINDOW *hw` for help/inventory):
`newwin`, `delwin`, `subwin`, `wmove`, `waddch`, `waddstr`, `wrefresh`,
`wclear`, `werase`, `wclrtoeol`, `wprintw`, `mvwprintw`, `mvwaddch`,
`mvwin`, `clearok`, `touchwin`, `leaveok`, `idlok`, `wgetnstr`,
`wstandout`, `wstandend`, `mvwinch`

**Character read-back** (rogue reads screen to check what's at a position):
`inch`, `mvinch`, `mvwinch`

**Input:**
`getch` (via `md_readchar` in mdport.c)

**Attributes:**
`standout`, `standend`, `wstandout`, `wstandend`

**Mode setting:**
`raw`, `noecho`, `keypad`, `nocbreak`

**Globals/macros:**
`stdscr`, `curscr`, `LINES`, `COLS`, `WINDOW`, `A_CHARTEXT`,
`getyx`, `getmaxy`, `getmaxx`, `mvcur`, `isendwin`, `unctrl`,
`erasechar`

**Not used:** `cbreak`, `nodelay`, `curs_set`, `start_color`, `has_colors`,
`COLOR_PAIR`, `beep`, `flash`, `napms`, `attron`/`attroff`/`attrset`.

#### 1b. Signals

All signal handling lives in `mdport.c` behind wrapper functions:

| Wrapper | Signals | Used for |
|---------|---------|----------|
| `md_onsignal_autosave()` | SIGHUP, SIGQUIT, SIGINT, SIGTERM, SIGILL, SIGTRAP, SIGFPE, SIGBUS, SIGSEGV | Normal gameplay |
| `md_onsignal_exit()` | Same set | Score display |
| `md_onsignal_default()` | Same set | Reset to SIG_DFL |
| `md_ignoreallsignals()` | 0..NSIG | During score write |
| `md_start_checkout_timer()` | **SIGALRM + alarm()** | Load-check timeout (CHECKTIME feature) |
| `md_tstphold/resume/signal()` | **SIGTSTP** | Job control suspend |

**PPAP impact:**
- `signal()` — works via musl (maps to `rt_sigaction`)
- `SIGALRM` / `alarm()` — **not implemented in PPAP**; used only when
  `CHECKTIME` is defined (multi-user load limiting). **Solution: don't
  define CHECKTIME** — the alarm code is ifdef'd out.
- `SIGTSTP` — not implemented; `md_tstphold()` falls back to `SIG_IGN`
  when `SIGTSTP` is undefined. **No action needed.**
- `SIGHUP` — not implemented; can `#define SIGHUP SIGTERM` or stub.

#### 1c. Process management

| Call | Location | Purpose | PPAP impact |
|------|----------|---------|-------------|
| `fork()`+`execl()`+`wait()` | `md_shellescape()` | Shell escape (`!` command) | Works (vfork semantics) |
| `getpid()` | `md_getpid()` | RNG seed | Works |
| `getuid()`/`getgid()` | `md_normaluser()` | Drop setgid privileges | Returns 0 (stub) |
| `getpwuid()` | `md_getusername()` | Get player name | **Missing in PPAP**; stub to return "player" |
| `getenv()` | main.c | ROGUEOPTS, TERM, SEED | Works via musl |

#### 1d. File I/O

| Operation | Purpose | PPAP impact |
|-----------|---------|-------------|
| `fopen`/`fread`/`fwrite`/`fclose` | Score file, save file | Works via musl stdio |
| `stat()` | Save file validation | Works |
| `chmod()` | Set score file 0664 | Stub (returns 0) |
| `unlink()` | Delete save file after restore | Works |
| `fopen(UTMP)` | Count logged-in users | **CHECKTIME only** — disabled |

Score file: path set by `SCOREFILE` compile-time macro. If undefined,
`scoreboard` is NULL and all score I/O is skipped.

Save file: defaults to `~/rogue.save`. Full game state (~40–60 KB
encrypted) written via `rs_save_file()` in state.c.

#### 1e. Memory layout

THING union (monster/object): **68 bytes** on ARM (verified from struct).
PLACE (map cell): **8 bytes** (char + char + pointer + padding).

| Data | Size |
|------|------|
| `places[32*80]` (level map) | 20 KB |
| String buffers (6 x MAXSTR=1024) | 6 KB |
| `prbuf[2*MAXSTR]` | 2 KB |
| Monster/item info tables | ~2 KB |
| Daemon list (20 slots) | ~1 KB |
| Runtime monsters (~20 THINGs) | ~1.4 KB |
| Runtime objects (~30 THINGs) | ~2 KB |
| Score table (10 x 1044) | 10 KB |
| **Total .data+.bss+heap** | **~45 KB** |

Fits in 128 KB. **MAXSTR=1024 is wasteful** — can reduce to 256 to save
~5 KB if needed, but not critical.

#### 1f. Other findings

- **Custom crypt**: `xcrypt.c` provides its own DES crypt — no libc
  `crypt()` needed. Used only for wizard-mode password.
- **`curscr->_cury`/`_curx`**: `main.c:241-242` accesses ncurses internal
  struct fields directly. Known upstream issue (BUILD_ISSUES.md). Must
  replace with `getyx(curscr, ...)` in our shim.
- **`mvcur()`**: Used in `main.c` and `save.c` to position physical cursor.
  Must implement in shim (trivial — same as `move()`).
- **`unctrl()`**: Returns printable representation of control chars.
  Must implement in shim (~10 lines).
- **`erasechar()`**: Returns terminal erase char. Stub to return `'\b'`.
- **`htonl`/`ntohl`**: Used in `state.c` for portable save files.
  musl provides these via `<arpa/inet.h>`.
- **No `time_t` wall clock**: RNG seed uses `time(NULL)` — PPAP returns
  uptime, which is fine for seeding.

### Step 2: Build a tiny curses shim

Rather than porting full ncurses (~100K+ lines), build a minimal curses
implementation that covers only the calls rogue actually uses. This is a
common approach for embedded rogue ports.

**File:** `third_party/rogue-curses/curses.h` + `curses.c`

The shim must support:

1. **stdscr + one extra WINDOW** (the `hw` scratch window)
2. **Shadow screen buffer** per window for `inch`/`mvinch`/`mvwinch`
3. **Dirty tracking** for `refresh`/`wrefresh` (optional; can flush all)
4. **VT100 escape output** for cursor, clear, attributes

| curses call      | ANSI escape / implementation |
|------------------|------------------------------|
| `initscr()`      | set raw mode via `tcsetattr`, clear screen, alloc stdscr |
| `endwin()`       | restore cooked mode, reset attributes, `ESC[?25h` |
| `clear()`        | `ESC[2J ESC[H`, zero shadow buffer |
| `move(y,x)`      | set cursor position in WINDOW struct |
| `addch(c)`       | write to shadow buffer at cursor, advance |
| `addstr(s)`      | loop `addch` |
| `mvaddch(y,x,c)` | `move` + `addch` |
| `mvaddstr(y,x,s)`| `move` + `addstr` |
| `printw(fmt,..)` | `vsprintf` + `addstr` |
| `mvprintw(y,x,.)` | `move` + `printw` |
| `clrtoeol()`     | fill shadow to end of line with spaces |
| `refresh()`      | diff shadow vs displayed, emit `ESC[y;xH` + chars |
| `standout()`     | `ESC[7m` (reverse video) |
| `standend()`     | `ESC[0m` (reset) |
| `getch()`        | `read(0, &c, 1)` + arrow key ESC sequence parsing |
| `inch()`         | return shadow buffer at cursor |
| `mvinch(y,x)`    | return shadow buffer at (y,x) |
| `newwin(h,w,y,x)`| allocate WINDOW + shadow buffer |
| `delwin(w)`      | free WINDOW |
| `subwin(w,h,w,y,x)` | allocate WINDOW sharing parent's buffer |
| `w*` variants    | operate on specified WINDOW instead of stdscr |
| `clearok(w,f)`   | set flag to force full redraw on next refresh |
| `touchwin(w)`    | mark all lines dirty |
| `leaveok(w,f)`   | set flag (cursor position doesn't matter) |
| `idlok(w,f)`     | no-op (no hardware line insert/delete) |
| `mvwin(w,y,x)`   | move window origin |
| `mvcur(oy,ox,ny,nx)` | emit `ESC[ny+1;nx+1H` |
| `getyx(w,y,x)`   | read cursor from WINDOW struct |
| `getmaxy(w)`     | return window height |
| `getmaxx(w)`     | return window width |
| `raw()`/`noecho()` | `tcsetattr` with `~ICANON ~ECHO` |
| `keypad(w,f)`    | set flag to enable arrow key parsing |
| `nocbreak()`     | restore `ICANON` |
| `unctrl(c)`      | return `"^X"` string for control chars |
| `erasechar()`    | return `'\b'` |
| `isendwin()`     | return endwin-called flag |
| `wgetnstr(w,s,n)` | read string with echo and editing |

Shadow buffer: `chtype buf[LINES][COLS]` per WINDOW. `chtype` encodes
both character and attribute (reverse video). ~4 bytes * 24 * 80 = 7680
bytes for stdscr + ~7680 for hw = **~15 KB** total.

Approximate shim size: **~800–1200 lines of C**, **~4 KB code**.

### Step 3: Create build script

**File:** `third_party/build-rogue.sh`

Pattern follows `build-busybox.sh`:

```sh
#!/bin/bash
# Build rogue for PPAP (ARMv6-M Thumb / Cortex-M0+)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ROGUE_SRC="$SCRIPT_DIR/rogue"
ROGUE_OUT="$PROJECT_ROOT/build/rogue"
MUSL_SYSROOT="$PROJECT_ROOT/build/musl-sysroot"
CURSES_DIR="$SCRIPT_DIR/rogue-curses"

CC=arm-none-eabi-gcc
CFLAGS="-mthumb -mcpu=cortex-m0plus -march=armv6s-m \
  -fPIC -msingle-pic-base -mpic-register=r9 \
  -mno-pic-data-is-text-relative \
  -ffreestanding -Os \
  -isystem $MUSL_SYSROOT/include \
  -isystem $CURSES_DIR \
  -DHAVE_CONFIG_H"
LDFLAGS="-nostdlib -T $PROJECT_ROOT/third_party/configs/busybox.ld \
  -pie -L$MUSL_SYSROOT/lib"

# Compile rogue .c files + curses shim
# Link against musl libc + libgcc
# Strip and install to $ROGUE_OUT/rogue
```

### Step 4: PPAP patches (overlay)

**Directory:** `third_party/patches/rogue/overlay/`

Create a `config.h` (the autoconf-generated header) with PPAP-specific
defines:

```c
/* PPAP config.h for rogue */
#define HAVE_FORK 1
#define HAVE_GETUID 1
#define HAVE_GETGID 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_STRING_H 1
#define HAVE_STDLIB_H 1
#define HAVE_CURSES_H 1
#define HAVE_TERMIOS_H 1
/* No CHECKTIME — disables alarm()/SIGALRM load checking */
/* No SCOREFILE — disables score file (initially) */
/* No HAVE_GETPWUID — stub returns "player" */
/* No HAVE_NLIST_H — no /dev/kmem load average */
/* No HAVE_UTMP_H — no user counting */
#define MASTER 0           /* disable wizard mode */
```

**mdport.c patches** (via overlay or sed):
- `md_getusername()`: return `"player"` when `!HAVE_GETPWUID`
- `md_gethomedir()`: return `"/tmp"` when `!HAVE_GETPWUID`
- `md_shellescape()`: can disable (`#if 0`) or keep (busybox shell works)

**main.c patch**:
- Replace `curscr->_cury` / `curscr->_curx` with `getyx(curscr, cy, cx)`

**Score/save strategy**:
- Initially: no SCOREFILE, save to `/tmp/rogue.save` (lost on reboot)
- PicoCalc: redirect to `/mnt/sd/rogue.save` and `/mnt/sd/rogue.score`

### Step 5: Integration with PPAP build

Update `CMakeLists.txt` to:

1. Call `third_party/build-rogue.sh` as a custom command
2. Install `build/rogue/rogue` to `romfs/bin/rogue`
3. Add dependency so romfs regeneration picks it up

### Step 6: Terminal environment setup

Update `romfs/etc/profile` to:

```sh
export TERM=vt100
```

This ensures musl's `getenv("TERM")` returns a value and any termcap
lookups (if used) match VT100 capabilities.

### Step 7: Test on QEMU

```sh
./scripts/qemu.sh
# at the shell prompt:
rogue
```

Verify:
- Screen clears and dungeon is drawn
- Player movement (hjkl / arrow keys) works
- Monsters, items, doors render correctly
- `standout` (reverse video) highlights visible
- Ctrl-C quits cleanly (SIGINT → endwin → exit)
- No memory exhaustion (watch `/proc/meminfo`)

### Step 8: Test on hardware

Flash to Pico / PicoCalc and play over UART (minicom/screen at 115200 baud).
Verify rendering and input at hardware speed.

---

## Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Curses shim larger than expected (~50 functions) | Medium | Well-scoped by audit; most are thin wrappers |
| `curscr->_cury/_curx` internal access | Low | Replace with `getyx(curscr,...)` in shim |
| `getpwuid()` missing | Low | Stub to return "player" |
| Shadow buffer memory (~15 KB for 2 windows) | Medium | Acceptable; total still ~60 KB |
| `SIGHUP` not implemented | Low | Map to SIGTERM or stub |
| Score file too large for tmpfs | Low | Disable scores initially; 10 entries = ~10 KB |
| Save file ~40-60 KB vs 8 KB tmpfs | Medium | Disable save on romfs-only; use SD on PicoCalc |
| Arrow keys need escape sequence parsing | Medium | Parse `ESC[A/B/C/D` in `getch()` |
| `htonl`/`ntohl` in state.c | Low | musl provides via `<arpa/inet.h>` |

---

## File Summary

| File | Purpose |
|------|---------|
| `third_party/rogue/` | Upstream submodule (Davidslv/rogue) |
| `third_party/rogue-curses/curses.h` | Minimal curses shim header |
| `third_party/rogue-curses/curses.c` | Minimal curses shim implementation |
| `third_party/build-rogue.sh` | Cross-compile build script |
| `third_party/patches/rogue/overlay/` | PPAP-specific patches |
| `romfs/bin/rogue` | Installed ELF binary |
| `docs/port-rogue.md` | This document |