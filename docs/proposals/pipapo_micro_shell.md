# push — PiPAPo μShell

## Overview

**push** (PiPAPo μShell) is a minimal, purpose-built shell for PiPAPo.
Busybox hush is functional but large (200–400 KB statically linked with musl)
and not easily customizable for PiPAPo's multi-subsystem architecture
(CP/M, Human68k, SOS, etc.). push targets <8 KB code + <4 KB data on ARM Thumb,
runs without malloc, and includes features tailored to PiPAPo — notably
case-insensitive PATH search for retro subsystem compatibility.

## Design Principles

1. **Static memory only** — no heap allocation; all buffers are fixed-size.
2. **No libc dependency** — can be built freestanding (like `init.c`) or
   optionally linked with musl.
3. **POSIX-inspired, not POSIX-compliant** — implements a practical subset.
4. **Single source file** — `src/user/push.c` (plus optional `push_line.c`
   for line editing if size warrants splitting).

## Resource Budget

| Resource        | Limit       | Notes                              |
|-----------------|-------------|------------------------------------|
| Code size       | <8 KB       | ARM Thumb, -Os                     |
| BSS/data        | <4 KB       | All static buffers                 |
| Stack           | 1 page (4 KB) | Standard user stack              |
| Total pages     | 3–4 pages   | Code + data + stack (+ heap page if musl) |
| Max line length | 256 bytes   | Input line buffer                  |
| Max argv        | 32 entries  | Arguments per command              |
| Max env vars    | 64 entries  | Combined local + global            |
| History depth   | 32 entries  | Ring buffer                        |
| Capture buffer  | 256 bytes   | `$(...)` output capture            |
| Max pipeline    | 4 stages    | `a | b | c | d`                   |

## Features

### 1. Environment Variables

Two-tier environment model:

- **Global (exported)**: inherited by child processes via `execve` environ.
- **Local (shell-only)**: visible in expansion but not passed to children.

Operations:
```sh
VAR=value           # set local variable
export VAR=value    # set and export (global)
export VAR          # promote existing local to global
unset VAR           # remove from both local and global
set                 # list all variables (local + global)
env                 # list exported variables only (builtin)
```

Variable expansion:
- `$VAR` — expand variable (global or local, local takes precedence)
- `${VAR}` — explicit delimited form
- `$?` — exit status of last command
- `$$` — PID of the shell process
- `$0` — name of the shell or script
- `$1`..`$9`, `$#`, `$@` — positional parameters (script/function args)

### 2. Current Directory Management

```sh
cd [dir]            # change directory (syscall: chdir)
cd                  # cd $HOME (or / if HOME unset)
cd -                # cd to previous directory ($OLDPWD)
pwd                 # print working directory (syscall: getcwd)
```

The shell tracks `$PWD` and `$OLDPWD` internally, updated on every
successful `chdir`.

### 3. I/O Redirection

Supported redirections (applied left-to-right before exec):

| Syntax     | Action                                      |
|------------|---------------------------------------------|
| `> file`   | Redirect stdout to file (truncate)          |
| `>> file`  | Redirect stdout to file (append)            |
| `< file`   | Redirect stdin from file                    |
| `2> file`  | Redirect stderr to file (truncate)          |
| `2>> file` | Redirect stderr to file (append)            |
| `2>&1`     | Redirect stderr to stdout                   |
| `1>&2`     | Redirect stdout to stderr                   |

Implementation: `open` + `dup2` in the child process between `vfork` and
`execve`. Since PiPAPo uses `vfork`, redirections must be applied carefully
— only `open`/`dup2`/`close`/`execve` are safe between `vfork` and `exec`.

### 4. Pipes

```sh
cmd1 | cmd2 | cmd3
```

- Up to 4 pipeline stages.
- Each stage is a separate `vfork` + `execve`.
- The shell waits for all stages to complete; `$?` reflects the exit status
  of the **last** command in the pipeline (like bash/POSIX).
- Pipe implementation: `pipe` + `dup2` for fd 0/1 in each stage.

### 5. Command Search (PATH Resolution)

When a command name contains no `/`, the shell searches `$PATH` directories
in order. Within each directory, candidates are matched with a 4-tier
priority:

| Priority | Match Rule                        | Example: input `hello`            |
|----------|-----------------------------------|-----------------------------------|
| 1 (best) | Exact filename                    | `hello`                           |
| 2        | Case-insensitive exact filename   | `Hello`, `HELLO`                  |
| 3        | Exact basename (strip extension)  | `hello.x`, `hello.com`           |
| 4        | Case-insensitive basename         | `Hello.X`, `HELLO.COM`           |

Rationale: retro subsystems store executables in upper-case with extensions
(e.g., `HELLO.COM` on CP/M, `HELLO.X` on Human68k). This allows users to
type `hello` naturally while finding the correct binary.

Implementation:
- For each `$PATH` directory, `opendir` + `readdir` and score each entry.
- Return the highest-priority match found in the first directory that has
  any match. Do not search further directories once a match is found.
- If the command contains `/`, use it as a literal path (no search).

### 6. Line Editing

VT100/ANSI terminal line editor, operating on raw terminal input
(`termios` raw mode or equivalent).

| Key            | Action                             |
|----------------|------------------------------------|
| Left / Ctrl-B  | Move cursor left                  |
| Right / Ctrl-F | Move cursor right                 |
| Home / Ctrl-A  | Move to start of line             |
| End / Ctrl-E   | Move to end of line               |
| Backspace      | Delete character before cursor    |
| Delete / Ctrl-D| Delete character at cursor (EOF if empty) |
| Ctrl-K         | Kill to end of line               |
| Ctrl-U         | Kill to start of line             |
| Ctrl-W         | Kill word backward                |
| Ctrl-L         | Clear screen, redraw prompt       |
| Ctrl-C         | Discard line (SIGINT)             |
| Up / Ctrl-P    | Previous history entry            |
| Down / Ctrl-N  | Next history entry                |
| Tab            | Filename completion (basic)       |
| Enter          | Execute line                      |

Insert mode only (no overwrite toggle). Terminal width is assumed 80 columns;
no multi-line wrapping support initially.

Tab completion:
- Completes file/directory names in the current word position.
- If only one match, insert it directly (append `/` for directories).
- If multiple matches, insert common prefix; second Tab lists candidates.
- No command-name completion (keeps it simple).

### 7. Command History

- Ring buffer of 32 entries (configurable at compile time).
- Navigated with Up/Down arrow keys.
- `history` builtin lists all entries.
- History is **not** persisted to disk (RAM-only, lost on shell exit).
- Duplicate consecutive entries are suppressed.

### 8. Scripting

push scripts begin with the shebang `#!/bin/push` (the kernel's existing
execve shebang handling invokes push with the script path as `$1`).

#### Supported Constructs

**Comments:**
```sh
# this is a comment
```

**Sequential execution:**
```sh
cmd1
cmd2
cmd3
```

**Command chaining:**
```sh
cmd1 && cmd2        # run cmd2 only if cmd1 succeeds (exit 0)
cmd1 || cmd2        # run cmd2 only if cmd1 fails (exit != 0)
cmd1 ; cmd2         # run cmd2 regardless of cmd1 exit status
```

Precedence: `&&` and `||` bind tighter than `;`. Evaluation is
left-to-right with short-circuit semantics. Pipes bind tightest.

**Conditionals:**
```sh
if cmd; then
  body
elif cmd; then
  body
else
  body
fi
```

The condition is any command; its exit status determines the branch.
Use the builtin `[[ ]]` for fast conditionals, or the external
`test` / `[` commands for POSIX compatibility.

**Loops:**
```sh
while cmd; do
  body
done
```

`break` and `continue` are supported. No `for` loops initially.

**Command substitution (single-level):**
```sh
VAR=$(cmd args)     # capture stdout of cmd into VAR
echo "host is $(hostname)"
```

- Nesting `$(cmd1 $(cmd2))` is **not** supported.
- Output is captured into a 256-byte fixed buffer.
- Trailing newlines are stripped.
- Implementation: `pipe` + `vfork` + `execve`, parent reads pipe to buffer,
  `waitpid` for child.

**Quoting:**

| Syntax       | Behavior                                       |
|--------------|-------------------------------------------------|
| `"..."`      | Double-quote: variable expansion, no globbing   |
| `'...'`      | Single-quote: literal, no expansion              |
| `\x`         | Backslash: escape next character                 |

No glob/wildcard expansion (`*`, `?`) — the shell passes arguments literally.
If globbing is needed in future, it would be added as a later enhancement.

**Positional parameters:**
```sh
#!/bin/push
echo "script: $0"
echo "first arg: $1"
echo "all args: $@"
echo "count: $#"
```

#### Scripting Example

```sh
#!/bin/push
# /etc/rc — system init script for PiPAPo

export PATH=/bin:/sbin:/usr/bin
export HOME=/root
export TERM=vt100

mount -t devfs none /dev
mount -t procfs none /proc
mount -t tmpfs none /tmp

if [[ -f /etc/hostname ]]; then
  HOSTNAME=$(cat /etc/hostname)
  export HOSTNAME
fi

echo "PiPAPo booting on $HOSTNAME" > /dev/console

# mount SD card if present
mount -t vfat /dev/sd0 /mnt || echo "no SD card"

exec getty /dev/ttyS0
```

### 9. Builtin Commands

Commands executed within the shell process (no fork/exec):

| Command            | Description                                  |
|--------------------|----------------------------------------------|
| `cd [dir]`         | Change directory                             |
| `pwd`              | Print working directory                      |
| `exit [N]`         | Exit shell with status N (default 0)         |
| `export [VAR=val]` | Export variable to environment               |
| `unset VAR`        | Remove variable                              |
| `set`              | List all variables                           |
| `env`              | List exported variables                      |
| `history`          | List command history                         |
| `echo [args...]`   | Print arguments (supports `-n`)              |
| `true`             | Exit status 0                                |
| `false`            | Exit status 1                                |
| `exec cmd`         | Replace shell with cmd (no fork)             |
| `.` / `source`     | Execute script in current shell context      |
| `[[ expr ]]`       | Builtin test (ksh-style, see below)          |

### 10. Builtin Test: `[[ ]]`

push provides `[[` ... `]]` as a **shell keyword** (ksh/bash-style) for
conditional tests. This avoids `vfork` + `execve` overhead on every `if`
condition — significant on RP2040 where fork is expensive.

`[` and `test` remain available as **external commands** (e.g., from busybox
or a standalone binary), preserving compatibility and allowing
subsystem-specific test implementations.

```sh
if [[ -f /etc/hostname ]]; then ...    # builtin, no fork (~700 bytes)
if [ -f /etc/hostname ]; then ...      # external /bin/test, fork+exec
if test -f /etc/hostname; then ...     # external /bin/test, fork+exec
```

#### Supported Expressions

**File tests** (argument: path, evaluated via `stat` syscall):

| Operator  | True if                            |
|-----------|------------------------------------|
| `-e file` | File exists                        |
| `-f file` | Regular file                       |
| `-d file` | Directory                          |
| `-r file` | Readable                           |
| `-w file` | Writable                           |
| `-x file` | Executable                         |
| `-s file` | Size > 0                           |

**String tests:**

| Operator         | True if                          |
|------------------|----------------------------------|
| `-z string`      | String is empty                  |
| `-n string`      | String is non-empty              |
| `s1 = s2`        | Strings are equal                |
| `s1 != s2`       | Strings are not equal            |

**Integer tests:**

| Operator         | True if                          |
|------------------|----------------------------------|
| `n1 -eq n2`      | Equal                            |
| `n1 -ne n2`      | Not equal                        |
| `n1 -lt n2`      | Less than                        |
| `n1 -gt n2`      | Greater than                     |
| `n1 -le n2`      | Less or equal                    |
| `n1 -ge n2`      | Greater or equal                 |

**Logical operators:**

| Operator         | Meaning                          |
|------------------|----------------------------------|
| `! expr`         | Negation                         |
| `expr && expr`   | Logical AND (short-circuit)      |
| `expr \|\| expr` | Logical OR (short-circuit)       |
| `( expr )`       | Grouping                         |

Because `[[` is a shell keyword (not a command), **no quoting is needed
around variable expansions** — `[[ -f $FILE ]]` works even if `$FILE`
contains spaces, since the parser handles `[[`...`]]` before word splitting.

Estimated code size: ~700 bytes on ARM Thumb -Os.

### 11. Signal Handling

- **SIGINT (Ctrl-C)**: in interactive mode, discard current input line and
  print a new prompt. In a running child, deliver SIGINT to the child.
- **SIGQUIT (Ctrl-\\)**: ignored by the shell.
- **SIGTSTP (Ctrl-Z)**: ignored (no job control).
- **SIGCHLD**: default handling via `waitpid`.
- **SIGPIPE**: default (terminate) — relevant in pipelines.

### 12. Prompt

Default prompt:
```
push$ command
```

Customizable via `$PS1`. Supported escape sequences:

| Escape | Expansion           |
|--------|---------------------|
| `\w`   | Current directory   |
| `\u`   | Username            |
| `\h`   | Hostname            |
| `\$`   | `#` if root, `$` otherwise |

Default `PS1`: `\u@\h:\w\$ `

## Deliberately Omitted

These features are excluded to keep push micro-sized:

- **Job control** (`bg`, `fg`, `jobs`, Ctrl-Z) — foreground-only
- **Glob expansion** (`*`, `?`, `[...]`) — pass arguments literally
- **Functions** / `return`
- **`case` / `esac`**
- **`for` loops** — use `while` with shift
- **Here-documents** (`<<EOF`)
- **Arrays**
- **Arithmetic expansion** (`$(( ))`)
- **Nested command substitution** (`$(cmd $(cmd))`)
- **Backtick substitution** (`` `cmd` ``)
- **Aliases**
- **Tilde expansion** (`~` → `$HOME`)
- **History file persistence**
- **Multi-line input continuation** (`\` at end of line)

Any of these can be added later if needed, but the initial goal is the
smallest useful interactive + scripting shell.

## Implementation Plan

### Phase 1: Core Interpreter (non-interactive) ✓ DONE

- Tokenizer: split input into words, handle quoting and `$` expansion
- Command execution: `vfork` + `execve` with `waitpid`
- Builtins: `cd`, `pwd`, `exit`, `export`, `unset`, `set`, `echo`,
  `true`, `false`, `exec`, `source`, `[[ ]]`, `env`
- I/O redirection: `>`, `>>`, `<`, `2>`, `2>>`, `2>&1`, `1>&2`
- Chaining: `&&`, `||`, `;`
- Pipes: up to 4 stages
- Environment: local/global two-tier model
- PATH search: 4-tier priority matching
- Redirect save/restore for builtins

Deliverable: push can run scripts via `#!/bin/push`.

Binary size (ARM Thumb -Os):
| Section | Bytes |
|---------|-------|
| .text   | 8,497 |
| .data   |   276 |
| .bss    | 2,336 |

### Phase 2: Scripting Constructs

- `if` / `elif` / `else` / `fi`
- `while` / `do` / `done`, `break`, `continue`
- `$(...)` single-level command substitution
- Positional parameters: `$0`..`$9`, `$#`, `$@`

Deliverable: push can replace hush for init scripts.

### Phase 3: Interactive Mode

- Raw terminal input handling
- Line editing (cursor movement, kill, yank)
- Command history (ring buffer, Up/Down navigation)
- Tab completion (filenames)
- Prompt (`$PS1` with escape sequences)
- Signal handling (SIGINT for line discard)

Deliverable: push is usable as the login shell.

## File Layout

```
src/user/push.c         # main shell: tokenizer, executor, builtins,
                         # scripting, env, PATH search
src/user/push_line.c    # line editor + history + tab completion
                         # (split out to allow omitting in script-only builds)
src/user/push.h         # shared definitions between push.c and push_line.c
```

Build configurations:
- **Full** (interactive + scripting): `push.c` + `push_line.c`
- **Script-only** (for minimal systems): `push.c` only, no line editing,
  reads lines via simple `read` syscall loop

## Testing Strategy

- **Host tests**: tokenizer and variable expansion can be tested on host
  (pure C, no syscalls).
- **QEMU ARM tests**: script execution — run push scripts that produce
  expected output, compare against golden output.
- **QEMU m68k tests**: same script tests on m68k target.
- **Interactive tests**: manual testing on QEMU and RP2040 hardware.
