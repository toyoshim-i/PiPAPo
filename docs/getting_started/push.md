# push — PiPAPo μShell

## Overview

**push** (PiPAPo μShell) is the default shell for PiPAPo (`/bin/sh → push`).
It is a minimal, purpose-built shell that runs without malloc, targets
~14 KB code + ~4 KB data on ARM Thumb, and includes features tailored to
PiPAPo's multi-subsystem architecture (CP/M, Human68k, SOS, etc.) — notably
case-insensitive PATH search for retro subsystem compatibility.

Busybox hush is optionally available as `/bin/hush` for POSIX compatibility.

## Resource Budget

| Resource        | Limit         | Notes                                |
|-----------------|---------------|--------------------------------------|
| Code size       | ~14 KB        | ARM Thumb, -Os (push.c + push_line.c)|
| BSS/data        | ~4 KB         | All static buffers                   |
| Stack           | 1 page (4 KB) | Standard user stack                  |
| **Total pages** | **2 pages**   | 1 data + 1 stack (8 KB)              |
| Max line length | 256 bytes     | Input line buffer                    |
| Max argv        | 32 entries    | Arguments per command                |
| Max env vars    | 64 entries    | Combined local + global              |
| History depth   | 32 entries    | Packed circular buffer (1 KB pool)   |
| Capture buffer  | 256 bytes     | `$(...)` output capture              |
| Max pipeline    | 4 stages      | `a \| b \| c \| d`                  |
| Script buffer   | 2 KB          | Dynamic — allocated via brk() on first compound statement |

## Design Principles

1. **Static memory only** — no heap allocation in normal operation;
   the compound statement buffer is the sole exception (brk-allocated
   on first `if`/`while`, unused in interactive-only sessions).
2. **No libc dependency** — built freestanding (like `init.c`).
3. **POSIX-inspired, not POSIX-compliant** — implements a practical subset.
4. **Two source files** — `src/user/push.c` (core) and `src/user/push_line.c`
   (line editor, history, tab completion).

## Quick Usage

### Interactive Mode

```
PiPAPo:/# echo hello
hello
PiPAPo:/# cd /bin
PiPAPo:/bin# ls
hello  push  hush  ps  cat  ...
PiPAPo:/bin# cd
PiPAPo:/#
```

### Script Mode

```sh
#!/bin/push
export PATH=/bin:/sbin
echo "Hello from push script"
if [[ -f /etc/hostname ]]; then
  echo "Host: $(cat /etc/hostname)"
fi
```

## Features

### Environment Variables

Two-tier model: **global** (exported, inherited by children) and
**local** (shell-only, visible in expansion but not passed to children).

```sh
VAR=value           # set local variable
export VAR=value    # set and export (global)
export VAR          # promote existing local to global
unset VAR           # remove variable
set                 # list all variables (local + global)
env                 # list exported variables only
```

Variable expansion: `$VAR`, `${VAR}`, `$?` (exit status), `$$` (PID),
`$0` (shell name), `$1`..`$9` (positional parameters), `$#` (count),
`$@` (all args).

### I/O Redirection

| Syntax     | Action                                      |
|------------|---------------------------------------------|
| `> file`   | Redirect stdout to file (truncate)          |
| `>> file`  | Redirect stdout to file (append)            |
| `< file`   | Redirect stdin from file                    |
| `2> file`  | Redirect stderr to file (truncate)          |
| `2>> file` | Redirect stderr to file (append)            |
| `2>&1`     | Redirect stderr to stdout                   |
| `1>&2`     | Redirect stdout to stderr                   |

### Pipes

```sh
cmd1 | cmd2 | cmd3      # up to 4 pipeline stages
```

`$?` reflects the exit status of the last command (POSIX semantics).

### Command Search (PATH Resolution)

When a command contains no `/`, push searches `$PATH` with 4-tier priority:

| Priority | Match Rule                        | Example: input `hello`            |
|----------|-----------------------------------|-----------------------------------|
| 1 (best) | Exact filename                    | `hello`                           |
| 2        | Case-insensitive exact filename   | `Hello`, `HELLO`                  |
| 3        | Exact basename (strip extension)  | `hello.x`, `hello.com`           |
| 4        | Case-insensitive basename         | `Hello.X`, `HELLO.COM`           |

This allows typing `hello` to find `HELLO.COM` (CP/M) or `HELLO.X` (Human68k).
If the best match fails to execute (child exits 127), push automatically
retries with the next-best candidate from PATH (up to 4 retries).

#### PATHEXT — Restricting Extension Matching

When `$PATHEXT` is set, tiers 3 and 4 only match files whose extension
is listed in `$PATHEXT`. The format follows the Windows convention:
semicolon-separated, dot-prefixed, case-insensitive.

```sh
export PATHEXT=.COM;.OBJ;.X;.R
```

With this setting, `hello` matches `hello.com` or `HELLO.X` but **not**
`hello.txt` or `hello.dat`. If `$PATHEXT` is unset, all extensions are
accepted (original behavior).

### Command Chaining

```sh
cmd1 && cmd2        # run cmd2 only if cmd1 succeeds
cmd1 || cmd2        # run cmd2 only if cmd1 fails
cmd1 ; cmd2         # run cmd2 unconditionally
```

### Conditionals and Loops

```sh
if cmd; then
  body
elif cmd; then
  body
else
  body
fi

while cmd; do
  body
done
```

`break` and `continue` are supported. Command substitution: `VAR=$(cmd)`.

### Builtin Commands

| Command            | Description                                  |
|--------------------|----------------------------------------------|
| `cd [dir]`         | Change directory (`cd -` for previous)       |
| `pwd`              | Print working directory                      |
| `exit [N]`         | Exit shell with status N                     |
| `export [VAR=val]` | Export variable to environment               |
| `unset VAR`        | Remove variable                              |
| `set`              | List all variables                           |
| `env`              | List exported variables                      |
| `history`          | List command history                         |
| `echo [args...]`   | Print arguments (supports `-n`)              |
| `true` / `false`   | Exit status 0 / 1                            |
| `exec cmd`         | Replace shell with cmd                       |
| `. file`           | Source script in current context              |
| `[[ expr ]]`       | Builtin test (see below)                     |

### Builtin Test: `[[ ]]`

File tests: `-e`, `-f`, `-d`, `-r`, `-w`, `-x`, `-s`.
String tests: `-z`, `-n`, `=`, `!=`.
Integer tests: `-eq`, `-ne`, `-lt`, `-gt`, `-le`, `-ge`.
Logical: `!`, `&&`, `||`, `( )`.

### Line Editing

VT100/ANSI line editor in raw terminal mode:

| Key            | Action                              |
|----------------|-------------------------------------|
| Left / Ctrl-B  | Move cursor left                   |
| Right / Ctrl-F | Move cursor right                  |
| Home / Ctrl-A  | Move to start of line              |
| End / Ctrl-E   | Move to end of line                |
| Backspace      | Delete character before cursor     |
| Delete / Ctrl-D| Delete at cursor (EOF if empty)    |
| Ctrl-K         | Kill to end of line                |
| Ctrl-U         | Kill to start of line              |
| Ctrl-W         | Kill word backward                 |
| Ctrl-L         | Clear screen, redraw               |
| Ctrl-C         | Discard line (SIGINT)              |
| Up / Ctrl-P    | Previous history entry             |
| Down / Ctrl-N  | Next history entry                 |
| Tab            | Context-aware completion           |

Tab completion is context-aware:

- **Command position** (first word): searches builtins and `$PATH`
  directories for matching executables. Case-insensitive.
- **Argument position**: completes filenames in the current or specified
  directory.
- Single match inserts the completion with a trailing space (or `/` for
  directories). Multiple matches insert the longest common prefix;
  pressing Tab again lists all candidates.

### Prompt

Customizable via `$PS1`. Default: `PiPAPo:\w# ` (set in `/etc/profile`).

| Escape | Expansion           |
|--------|---------------------|
| `\w`   | Current directory   |
| `\u`   | Username            |
| `\h`   | Hostname            |
| `\$`   | `$` (or `#`)        |

### History

Packed circular buffer (1 KB pool, up to 32 entries). Duplicate
consecutive entries are suppressed. Not persisted to disk.

## Deliberately Omitted

Job control, glob expansion, functions, `case`/`esac`, `for` loops,
here-documents, arrays, arithmetic expansion, nested `$(...)`, aliases,
tilde expansion, history persistence, multi-line continuation.

## File Layout

```
src/user/push.c         # core: tokenizer, executor, builtins, scripting, env
src/user/push_line.c    # line editor, history, tab completion
src/user/push.h         # shared definitions
src/etc/profile          # default PS1 and PATH (sourced on interactive startup)
```

## See Also

- [Userland development guide](userland_dev_guide.md) — how to build user programs
- [Syscall reference](../kernel/syscall.md) — syscalls used by push
- [Build and run](build_and_run.md) — building and running PiPAPo
- [CP/M subsystem](../subsystems/cpm.md) — retro subsystem PATH search context
- [Human68k subsystem](../subsystems/human68k.md) — retro subsystem PATH search context
