# pi — PiPAPo Editor

A tiny, colorful, menu-driven text editor for PPAP user-space.  Vi-compatible
keybindings for power users, with an always-visible menu bar so newcomers can
discover every command without memorization.

---

## 1. Design Philosophy

- **Vi-compatible**: All keybindings follow standard vi.  Users who know vi
  never need the menu.
- **Menu-driven**: A top menu bar provides discoverability.  Every action can
  be reached via arrow-key navigation — no hidden commands.
- **Colorful**: Line-number gutter, colored status bar, mode indicator, and
  syntax-highlighted UI elements match the PPAP app style (hello, top, ps).
- **64 KB budget**: Gap buffer, no undo tree (single-level `u`), no regex, no
  multi-buffer.  Stripped binary target: 20–30 KB.

---

## 2. Screen Layout

```
 File  Edit  Search  Go  Help                     ← menu bar (reverse video)
  1│#include <stdio.h>                             ← line numbers: dim cyan
  2│
  3│int main(void) {
  4│    return 0;█                                  ← cursor
  5│}
~                                                  ← tilde past EOF: dim blue
~
── pi: hello.c ──── NORMAL ──── 4:12 ──           ← status bar (reverse video)
 Esc Menu │ i insert │ :w save │ :q quit │ / search  ← hint bar (dim)
```

### 2.1 Menu Bar

Always visible at line 1.  Categories: **File**, **Edit**, **Search**, **Go**,
**Help**.  Highlighted item tracks arrow-key navigation.

### 2.2 Dropdown Menus

Opened by pressing Esc (opens File) or navigating to a category and pressing
Down/Enter.  Each item shows the vi shortcut on the right:

```
 File  Edit  Search  Go  Help
┌────────────────┐
│ New        :e!  │
│ Open       :e   │
│ Save       :w   │
│ Save As    :w f │
│────────────────│
│ Quit       :q   │
│ Force Quit :q!  │
│ Save+Quit  :wq  │
└────────────────┘
```

### 2.3 Status Bar

Reverse-video bar showing filename, modified flag, mode, and cursor position:

- **Clean**: green reverse — `── pi: file.c ──── NORMAL ──── 4:12 ──`
- **Modified**: yellow reverse — `── pi: file.c [+] ──── INSERT ──── 4:12 ──`

### 2.4 Hint Bar

Context-sensitive bottom line (dim white):

| Mode | Hints |
|---|---|
| Normal | `Esc Menu │ i insert │ :w save │ :q quit │ / search` |
| Insert | `Esc normal │ type to edit` |
| Menu | `←→ category │ ↑↓ select │ Enter run │ Esc close` |
| Command | `Enter execute │ Esc cancel` |

---

## 3. Modes and State Machine

```
              Esc (from normal)
  NORMAL ──────────────► MENU (File dropdown open)
   │  ▲                  │  ▲
   │  │ Esc              │  │ Left/Right
   │  │◄─────────────────┘  │ (switch category)
   │                     ▼──┘
   │ i/a/o            MENU items
   ▼                  Up/Down/Enter
  INSERT
   │
   │ Esc
   ▼
  NORMAL
```

Additionally, `:` in normal mode enters **COMMAND** mode (bottom-line input
for `:w`, `:q`, `:e`, `:123`, etc.).

| Mode | Entry | Exit |
|---|---|---|
| NORMAL | Default / Esc from insert | Esc → menu, i/a/o → insert, : → command |
| INSERT | `i`, `a`, `o`, `O` from normal | Esc → normal |
| MENU | Esc from normal | Esc → normal, Enter → execute + normal |
| COMMAND | `:` from normal | Enter → execute + normal, Esc → normal |

---

## 4. Keybindings (Vi-Compatible)

### 4.1 Normal Mode

| Key | Action | Menu Location |
|---|---|---|
| `h` / `←` | Move left | — |
| `j` / `↓` | Move down | — |
| `k` / `↑` | Move up | — |
| `l` / `→` | Move right | — |
| `0` | Beginning of line | — |
| `$` | End of line | — |
| `w` | Next word | — |
| `b` | Previous word | — |
| `gg` | Go to top | Go → Top |
| `G` | Go to bottom | Go → Bottom |
| `i` | Insert before cursor | Edit → Insert |
| `a` | Append after cursor | Edit → Append |
| `o` | Open line below | Edit → Open Below |
| `O` | Open line above | Edit → Open Above |
| `x` | Delete char | Edit → Delete Char |
| `dd` | Delete line | Edit → Delete Line |
| `u` | Undo | Edit → Undo |
| `/` | Search forward | Search → Find |
| `n` | Next match | Search → Next |
| `N` | Previous match | Search → Prev |
| `:w` | Save | File → Save |
| `:q` | Quit | File → Quit |
| `:q!` | Force quit | File → Force Quit |
| `:wq` | Save and quit | File → Save+Quit |
| `:e file` | Open file | File → Open |
| `:e!` | New (discard) | File → New |
| `:N` | Go to line N | Go → Line... |
| `Esc` | Open File menu | — |
| `F1` | Help screen | Help → Keys |

### 4.2 Insert Mode

All printable keys insert text.  Backspace and Delete work as expected.
`Esc` returns to normal mode.

### 4.3 Menu Mode

| Key | Action |
|---|---|
| `←` / `→` | Switch category |
| `↑` / `↓` | Navigate items |
| `Enter` | Execute selected item |
| `Esc` | Close menu, back to normal |

---

## 5. Menus

### File
| Item | Shortcut |
|---|---|
| New | `:e!` |
| Open | `:e` |
| Save | `:w` |
| Save As | `:w f` |
| — | |
| Quit | `:q` |
| Force Quit | `:q!` |
| Save+Quit | `:wq` |

### Edit
| Item | Shortcut |
|---|---|
| Insert | `i` |
| Append | `a` |
| Open Below | `o` |
| Open Above | `O` |
| — | |
| Delete Char | `x` |
| Delete Line | `dd` |
| — | |
| Undo | `u` |

### Search
| Item | Shortcut |
|---|---|
| Find | `/` |
| Next | `n` |
| Prev | `N` |

### Go
| Item | Shortcut |
|---|---|
| Top | `gg` |
| Bottom | `G` |
| Line... | `:N` |

### Help
| Item | Shortcut |
|---|---|
| Keys | `F1` |
| About | |

---

## 6. Color Scheme

| Element | Color |
|---|---|
| Menu bar | Reverse video (white on blue) |
| Menu dropdown | Reverse video, selected item highlighted |
| Line numbers | Dim cyan |
| Tilde (past EOF) | Dim blue |
| Status bar (clean) | Reverse green |
| Status bar (modified) | Reverse yellow |
| Mode indicator | Bold cyan (`-- INSERT --`) |
| Hint bar | Dim white |
| Search match | Reverse yellow |
| Cursor (normal) | Block |
| Cursor (insert) | Bar (if terminal supports) |

---

## 7. Data Structures

```c
/* Gap buffer — O(1) insert/delete at cursor */
typedef struct {
    char *data;        /* heap via brk() */
    int   cap;         /* total allocation */
    int   gap_start;   /* cursor position */
    int   gap_end;     /* gap_start + gap_size */
} gap_buf_t;

/* Editor state — single global */
typedef struct {
    gap_buf_t  buf;
    char       filename[256];
    int        cx, cy;         /* cursor col, row in file */
    int        scroll_row;     /* first visible row */
    int        rows, cols;     /* terminal size (TIOCGWINSZ) */
    int        dirty;          /* unsaved changes flag */
    int        mode;           /* MODE_NORMAL / MODE_INSERT / MODE_MENU / MODE_CMD */
    int        menu_cat;       /* selected menu category (0=File..4=Help) */
    int        menu_item;      /* selected item within dropdown (-1 = bar only) */
    char       cmd[128];       /* command-line input for : commands */
    int        cmd_len;
    char       status[128];    /* status message */
    char       search[64];     /* last search term */
    /* single-level undo */
    char      *undo_snap;      /* snapshot before last edit */
    int        undo_len;
    int        undo_cx, undo_cy;
} editor_t;
```

---

## 8. Architecture

| File | Description | Est. |
|---|---|---|
| `src/user/pi/pi.c` | Main loop, mode dispatch, file load/save | ~2 KB |
| `src/user/pi/pi_buf.c` | Gap buffer (init, insert, delete, line nav, undo snapshot) | ~3 KB |
| `src/user/pi/pi_term.c` | Raw mode, key reader (arrows, Esc sequences, F-keys, Alt), ANSI output | ~3 KB |
| `src/user/pi/pi_ui.c` | Screen refresh — menu bar, content area, status bar, hint bar | ~3 KB |
| `src/user/pi/pi_menu.c` | Menu data tables, dropdown rendering, item dispatch | ~2 KB |
| `src/user/pi/pi.h` | Shared types and declarations | ~1 KB |

Estimated stripped binary: 20–30 KB (within 64 KB budget with uclib).

---

## 9. Build Integration

```cmake
# cmake/user.cmake
set(PPAP_USER_MAIN_SOURCE_pi ${PPAP_ROOT}/src/user/pi/pi.c)
set(PPAP_USER_EXTRA_SOURCES_pi
    ${PPAP_ROOT}/src/user/pi/pi_buf.c
    ${PPAP_ROOT}/src/user/pi/pi_term.c
    ${PPAP_ROOT}/src/user/pi/pi_ui.c
    ${PPAP_ROOT}/src/user/pi/pi_menu.c
)
```

Installed to `/bin/pi` in romfs.

---

## 10. Implementation Steps

| Step | What | Depends |
|---|---|---|
| **E-1** | `pi_term.c`: raw mode (TCGETS/TCSETS), key reader (arrows, Esc, F1, F10, Alt), TIOCGWINSZ | — |
| **E-2** | `pi_buf.c`: gap buffer (init/grow, insert char, delete char, delete line, line counting, get row content) | — |
| **E-3** | `pi_menu.c`: menu category/item tables, dropdown state, dispatch function table | — |
| **E-4** | `pi_ui.c`: full screen refresh — menu bar, content area with line numbers, tilde lines, status bar, hint bar, dropdown overlay | E-1 |
| **E-5** | `pi.c`: main loop, normal mode (hjkl, 0/$, w/b, gg/G, scroll), menu mode (Esc opens File, arrows navigate, Enter dispatches) | E-1..E-4 |
| **E-6** | Insert mode (`i`/`a`/`o`/`O`, typing, backspace, delete, Esc back to normal) | E-5 |
| **E-7** | File I/O: open (cmdline arg or `:e`), save (`:w`), save-as (`:w file`), quit with dirty check (`:q`/`:q!`/`:wq`), new (`:e!`) | E-6 |
| **E-8** | Command mode (`:` line input), search (`/`, `n`, `N`), go-to-line (`:N`), undo (`u`, single-level snapshot) | E-7 |
| **E-9** | Help screen (`F1` / Help → Keys), About dialog, polish and edge cases | E-8 |

---

## 11. Dependencies

- **uclib**: `uc_strlen`, `uc_strcmp`, `uc_strcpy`, `uc_memcpy`, `uc_memset`, `uc_snprintf`
- **syscall.h**: `read`, `write`, `open`, `close`, `lseek`, `ioctl` (TCGETS/TCSETS/TIOCGWINSZ), `brk`, `stat`
- **common/termios.h**: `struct termios`, flag constants
- No external dependencies.
