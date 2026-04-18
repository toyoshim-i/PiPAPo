# Host Target

A native build of selected PPAP user-space apps against host libc.
Produces Linux/macOS binaries that run directly, with no kernel, no
romfs, no QEMU.  Useful for iterating on the shell and editor without
flashing a board, and for running them as general-purpose dev tools.

---

## 1. What Builds

| Binary | Source | Notes |
|--------|--------|-------|
| `build/host/hello` | `src/user/hello.c` | Plumbing sanity check; no shim required. |
| `build/host/push` | `src/user/push.c` + `push_line.c` | Full μShell. Builtins, pipes, `$(...)`, `~/.pushrc`. |
| `build/host/pi` | `src/user/pi/*` | vi-like editor. Modes, gap buffer, ANSI UI. |

Kernel, other user apps (init, getty, ps, etc.), and romfs are
deliberately out of scope — the host build is for the interactive apps
that stand alone meaningfully outside the kernel.

---

## 2. Build and Run

```sh
./scripts/build.sh host       # native gcc/clang, no Docker
./build/host/push             # interactive μShell
./build/host/pi myfile.txt    # edit a file
```

`host` is deliberately **not** in `./scripts/build.sh all` — it's opt-in.

---

## 3. How the Shim Works

`src/target/host/` contains the entire port.  Shared kernel/user sources
are untouched.

### Include-path override

Apps `#include "syscall.h"` and `#include "common/termios.h"` etc.
These are shadowed under `src/target/host/include/`.  Two mechanisms
ensure the shadow wins:

1. `include_directories(BEFORE ...)` puts `target/host/include/` ahead
   of `src/` and `src/user/` on the search path for angle-bracket-style
   resolution.
2. `-include target/host/include/syscall.h` is force-applied to every
   app translation unit.  This is required because quoted-include
   (`#include "syscall.h"`) first searches the including file's
   directory, which would otherwise pick up `src/user/syscall.h` before
   our `-I` path is consulted.  The shadow file opens with
   `#define PPAP_USER_SYSCALL_H` so the app's later `#include` hits
   the original's guard and short-circuits.

The shadow `syscall.h` pulls in libc headers (`<unistd.h>`,
`<sys/stat.h>`, `<dirent.h>`, `<termios.h>`, `<poll.h>`, `<signal.h>`,
...).  PPAP's `struct stat`, `struct dirent`, `struct termios`,
`struct pollfd`, `ssize_t`, and `pid_t` become libc's natively — this
avoids every layout-mismatch class of bug at compile time.

### Adapter shim (`posix_shim.c`)

Five PPAP syscalls don't map 1-to-1 to libc; the shim provides
adapters.  The shadow redirects the PPAP names to `ppap_*` wrappers
via `#define` (so `ppap_shim.c` itself must not see the shadow —
`CMakeLists.txt` attaches the force-include per-source).

| Wrapper | Behavior |
|---------|----------|
| `ppap_getdents(fd, buf, count)` | Keyed `fd → DIR*` cache over `fdopendir(dup(fd))` + `readdir`. |
| `ppap_brk(addr)` | `NULL` → `sbrk(0)` query; otherwise `sbrk(delta)` to reach `addr`; returns new break. |
| `ppap_ppoll(fds, nfds, ts, mask, setsize)` | Drops `setsize` and forwards to libc 4-arg `ppoll`. |
| `ppap_sigaction(sig, handler, oldhandler)` | Wraps a raw function pointer into `struct sigaction`; returns previous handler pointer. |
| `ppap_getcwd(buf, size)` | Libc `char*`-or-`NULL` → PPAP `int` length-or-(-1). |

Everything else (`read`, `write`, `open`, `close`, `vfork`, `execve`,
`waitpid`, `ioctl`, `kill`, `stat`, `pipe`, `dup`, `dup2`, `chdir`,
`mkdir`, `unlink`, `clock_gettime`, `nanosleep`, `_exit`, ...)
resolves directly against libc by matching signatures.

### vfork

Linux `vfork(2)` is `clone(CLONE_VM | CLONE_VFORK | SIGCHLD)` — shared
VM and parent-blocked-until-exec, matching PPAP's semantics.  push's
three `vfork` sites (`$(…)` capture, single exec, pipeline stage)
work unchanged on Linux and macOS.  POSIX strictly forbids non-exec
function calls in the vfork child, but both OSes have documented
lenient behaviour for decades.

### ENV pool sizing

push's default `ENV_POOL_SIZE` (1.5 KB) is sized for the embedded
inittab-spawned environ.  A host dev shell's inherited environ is
typically 2–4 KB.  `CMakeLists.txt` overrides via
`target_compile_definitions(push PRIVATE ENV_POOL_SIZE=16384
ENV_MAX=256)`; push's source has `#ifndef` guards on those macros.

---

## 4. Behavioural Notes

- **Login shell:** `push -l` triggers login-shell mode (sources
  `/etc/profile`).  A plain `./build/host/push` is a non-login shell
  and does **not** touch the system profile — the host's `/etc/profile`
  is bash-syntactic and would produce spurious errors.
- **`~/.pushrc`:** sourced on every interactive session (login or not),
  analogous to bash's `~/.bashrc`.  Skipped silently when `$HOME` is
  unset.  A good place to set `PS1`, `PATH`, etc. for the host session.
- **PATH:** inherited from the parent shell.  The embedded default
  (`/bin:/sbin:/usr/bin:/subsys/...`) is not forced.
- **Line editing:** push uses raw-mode VT100 editing when `TERM` is
  set to anything except `dumb`.  Piping input into push for scripting
  works with `TERM=dumb`.

---

## 5. Adding a New App

1. Confirm its syscall usage matches what the shim provides.  `grep`
   for any syscall in `src/user/<app>.c` that isn't already handled
   (check `src/target/host/include/syscall.h` and `posix_shim.c`).
2. Add the source to `APP_SRCS` in `src/target/host/CMakeLists.txt`
   so the force-include picks it up.
3. Add an `add_executable(<app> ...)` stanza, linking
   `posix_shim.c` if the app uses any of `getdents / brk / ppoll /
   sigaction / getcwd`.

Apps that require `vfork/exec` trees of other PPAP binaries (e.g.,
init, getty) would need a rethink — on host they'd need to exec host
binaries, not PPAP user ELFs.  Porting them isn't meaningful.

---

## 6. File Layout

```
cmake/toolchain_host.cmake              Native CC, -DPPAP_HOST_BUILD, _GNU_SOURCE
src/target/host/
    CMakeLists.txt                      Project + three executables
    posix_shim.c                        Five adapters (talks to raw libc)
    include/
        syscall.h                       Shadow — force-included on every app TU
        common/fcntl.h                  Shadow → <fcntl.h>
        common/termios.h                Shadow → <termios.h>, <sys/ioctl.h>
```

---

## 7. Limitations

- **Linux/macOS only.**  No Windows support; `vfork`, `ppoll`,
  `fdopendir`, and `_GNU_SOURCE` flags assume a POSIX host.
- **Not a full PPAP environment.**  No procfs, no `/dev/tty*`, no
  inittab, no multi-tty.  Apps that expect those paths will simply
  see host equivalents (the real host `/proc` on Linux) or `ENOENT`.
- **PATHEXT** (retro-subsystem extension matching in push) has no
  meaning on host.  Leave it unset in `~/.pushrc`; the four-tier PATH
  search still works — tiers 3 and 4 just never match.
- **PicoCalc and LCD bits in pi** — none.  pi is target-agnostic; the
  kernel's tty driver is what routes VT100 escapes to the LCD on the
  embedded targets, and pi never sees the difference.
