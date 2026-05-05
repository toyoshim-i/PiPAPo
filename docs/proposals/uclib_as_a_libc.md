# Proposal: Promote `uclib` to a Minimal PPAP libc, Drop musl

## Summary

PPAP currently links against musl libc to support exactly one consumer:
Rogue 5.4.4.  Every other user-space program — the ~50 native applets and
the push shell — uses `uclib`, a small freestanding helper library with
`uc_*` prefixed symbols and PPAP-internal headers.

This proposal grows `uclib` into a real (but minimal) libc, exposed under
the standard POSIX header names, and migrates Rogue onto it so musl can be
removed entirely.

The aim is **not** a fully POSIX-conformant libc.  It is the smallest
libc that lets PPAP's userland and Rogue compile and run, written in
freestanding C and matched to the kernel's syscall surface.

Outcome: a single, in-tree libc with no submodule, no per-arch musl
patches, no separate sysroot — and a userland that uses `<stdio.h>`,
`<string.h>`, `<stdlib.h>` like any other C program.

## Motivation

Reasons to do this:

- **Build / setup cost.**  musl is a submodule, has a custom per-arch
  build script (`third_party/build_musl.sh`), and produces a sysroot of
  ~1 MB per architecture.  ePIC RISC-V additionally carries a patched
  CRT and a custom linker script.  All of that exists to support one
  third-party app.
- **Toolchain simplicity.**  Removing musl means each user program is
  compiled the same way (freestanding, `-nostdlib`, in-tree headers).
  The "musl-linked vs uclib-linked" split disappears.
- **Coherence.**  Today the native applets call `uc_strlen`, `uc_printf`,
  etc., while ports call `strlen`, `printf`.  Two parallel idioms exist
  for the same functions.  One libc with standard names removes that
  split.
- **Footprint control.**  A purpose-built libc lets us decide what to
  ship and what to skip (no locale, no wide-char, no NSS, no dlfcn).
  musl is small but it is not microcontroller-small.

Reasons not to do this:

- **Effort.**  Writing libc functions is unglamorous, and the FILE
  stream layer is non-trivial.  Estimated total: ~2000 net lines added
  to PPAP, ~10000+ lines of musl removed.
- **Compatibility tax.**  Future ports of unmodified third-party apps
  may pull in a libc surface we have not implemented.  We would either
  add the missing piece or patch the port.

The trade-off looks favorable: the cost is bounded and one-time; the
benefit (removing a submodule, a build phase, and a parallel idiom) is
permanent.

## Goals

- Provide POSIX-named headers (`<stdio.h>`, `<string.h>`, `<stdlib.h>`,
  `<unistd.h>`, `<errno.h>`, `<signal.h>`, `<time.h>`, `<ctype.h>`,
  `<setjmp.h>`, plus `<sys/stat.h>`, `<sys/wait.h>`, etc.).
- Migrate every native applet to standard names; drop the `uc_*` prefix.
- Build Rogue against the new libc with no patches beyond the existing
  curses shim.
- Remove the musl submodule, build script, sysroot, linker scripts, and
  related test machinery once Rogue is migrated.

## Non-Goals

- A POSIX-conformant libc.  We are matching what PPAP's userland and
  Rogue actually use, not a spec.
- Locale support, wide-char (`wchar_t` / `<wchar.h>`), `iconv`, NSS,
  dynamic linking, regular `<regex.h>` (sed has its own BRE engine).
- Threading.  PPAP user space is single-threaded; pthreads stays out.
- Floating-point printf precision matching glibc.  Existing `uc_printf`
  semantics are sufficient.

## Header Layout

User programs are compiled with a new system include path:

```
-isystem src/user/include
```

`src/user/include/` holds POSIX-named headers.  Some are forwarding
shims into `src/common/` (which already defines the kernel-shared ABI
types like `struct stat`, `struct dirent`); others are written from
scratch for `uclib`'s domain.

**No symlinks.**  Each forwarding header is a real file that
`#include`s the matching `src/common` header.  This keeps the existing
inter-module dependency convention explicit (user is allowed to depend
on common; the forwarding direction is recorded in the header itself).

### Forwarding headers (just delegate to `src/common/`)

```c
// src/user/include/dirent.h
#ifndef _DIRENT_H
#define _DIRENT_H
#include "common/dirent.h"
#endif

// src/user/include/sys/stat.h
#ifndef _SYS_STAT_H
#define _SYS_STAT_H
#include "common/stat.h"
#endif
```

Mapping:

| POSIX header             | Forwards to              |
| ------------------------ | ------------------------ |
| `<dirent.h>`             | `common/dirent.h`        |
| `<fcntl.h>`              | `common/fcntl.h`         |
| `<poll.h>`               | `common/poll.h`          |
| `<sys/stat.h>`           | `common/stat.h`          |
| `<sys/statfs.h>`         | `common/statfs.h`        |
| `<sys/uio.h>`            | `common/iovec.h`         |
| `<sys/wait.h>`           | `common/wait.h`          |
| `<sys/utsname.h>`        | `common/utsname.h`       |
| `<sys/mount.h>`          | `common/mount.h`         |
| `<sys/ptrace.h>`         | `common/ptrace.h`        |

### New headers (no `src/common` counterpart)

`<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<ctype.h>`, `<errno.h>`,
`<signal.h>`, `<time.h>`, `<unistd.h>`, `<setjmp.h>`, `<assert.h>`.

`<unistd.h>` exposes the syscall wrappers currently declared in
`src/user/syscall.h` (`read`, `write`, `open`, `close`, `lseek`,
`getpid`, `fork`, `execve`, …).

### Implementation files

`src/user/lib/uclib.c` is split, by header, into:

```
src/user/lib/string.c   strlen, strcmp, memcpy, memmove, strstr, ...
src/user/lib/stdio.c    putchar, puts, printf, snprintf, vsnprintf, ...
src/user/lib/stdlib.c   malloc, free, qsort, atoi, strtol, getenv, ...
src/user/lib/ctype.c    isalpha, isdigit, ... (or table-only header)
src/user/lib/errno.c    int errno; (single TU owns the storage)
src/user/lib/signal.c   signal() wrapper over rt_sigaction
src/user/lib/time.c     time, clock, gmtime, localtime, mktime, strftime
src/user/lib/setjmp.S   per-arch setjmp/longjmp
src/user/lib/file.c     FILE streams (M4)
```

## Milestones

Each milestone is one or more commits ending in a working tree.  The
roman-numeral order is the recommended order; M3/M4/M5 are
independent and can be done in any order after M2.

### M1 — Header reorg, behavior unchanged

- Add `src/user/include/` with forwarding headers and skeleton headers.
  All standard names are declared as aliases of the existing `uc_*`
  symbols (`#define strlen uc_strlen` or extern decls + same body).
- Add `-isystem src/user/include` to user-program compile flags in
  `cmake/user.cmake`.
- No source files change.  Existing applets keep using `uc_*` names.
- Verify: `qemu_arm` and `qemu_m68k` test suites pass; per-app binary
  sizes are byte-identical to pre-M1 (proves it is purely additive).

### M2 — Rename `uc_*` to standard names

- Sweep `src/user/*.c` and `src/user/lib/*` to replace every `uc_X`
  with `X`.  This is mechanical: the alias header from M1 already made
  the new names available.
- Split `uclib.c` into `string.c`, `stdio.c`, `stdlib.c`, etc.
- Drop the `uc_*` aliases.  Update header guards from `PPAP_USER_LIB_*`
  to `_STDIO_H` style.
- Verify: all 24 applets + tests build and pass on `qemu_arm` and
  `qemu_m68k`.

### M3 — Standard library gaps (lightweight)

- `errno` storage: `src/user/lib/errno.c` defines `int errno;`.
  Syscall wrappers stay returning `-errno`-style negatives for now;
  M3 only declares the variable and the canonical names from
  `<errno.h>` (`EINVAL`, `ENOENT`, …).  An optional pass converts
  the wrappers to set `errno` and return `-1`, gated on whether
  Rogue actually needs it (it does for `fopen` errors).
- Add: `strtol`, `strtoul`, `qsort`, `bsearch`, `abs`, `labs`,
  `memmove`, `strstr`, `strspn`, `strcspn`, `strpbrk`, `strdup`.
- Full `<ctype.h>` (probably as `static inline` or a 256-byte table).
- Estimated +500 LOC.

### M4 — `<stdio.h>` FILE streams

- `FILE` struct: fd + small buffer + position + flags + 1-byte ungetc.
- `fopen`, `fclose`, `fread`, `fwrite`, `fprintf`, `fputs`, `fgets`,
  `getc`, `putc`, `ungetc`, `feof`, `ferror`, `fflush`, `setvbuf`,
  `clearerr`, `rewind`, `fseek`, `ftell`.
- `stdin`, `stdout`, `stderr` globals (point at fd 0/1/2 with line
  buffering on stdout, unbuffered stderr).
- `fscanf` / `sscanf` minimum viable subset (`%d`, `%s`, `%c`, `%x`).
- Skip: `freopen`, `tmpfile`, `mkstemp`, locale-aware printf flags,
  `%a`, `%n`.
- Estimated +800 LOC, mostly in `src/user/lib/file.c`.

### M5 — `<signal.h>`, `<time.h>`, `<setjmp.h>`

- `signal()` wrapping `rt_sigaction` (already syscall-exposed).
- `time()`, `clock()`; `gmtime`, `localtime`, `mktime`, `difftime`,
  `strftime`.  `uc_gmtime` exists; reshape it.
- `setjmp` / `longjmp`: per-arch assembly stubs (`src/arch/<arch>/user/setjmp.S`).
  Tiny — saves callee-saved regs + sp + return addr.
- Estimated +300–500 LOC.

### M6 — Rogue migration

1. Run `nm -D third_party/.../rogue` (or the link map) to enumerate
   musl symbols Rogue actually references.
2. Diff against the libc surface from M2–M5; close any remaining
   gaps with targeted additions.
3. Rebuild Rogue with `-nostdlib -isystem src/user/include` and the
   PPAP libc (link against the existing user `crt0.S` plus the new
   libc TUs as a static archive or just object list).
4. Boot, run, win/lose a game on `qemu_arm`.

If Rogue pulls in a symbol that is genuinely heavy and rarely used
(e.g. floating-point `printf` widths, locale stubs), the fallback is
to patch Rogue's call site rather than implement the heavy version.
We control Rogue's tree via `third_party/patches/rogue/`.

### M7 — Drop musl

- Remove `third_party/musl` submodule (deinit, drop from
  `.gitmodules`, `git rm --cached`).
- Delete `third_party/build_musl.sh`,
  `third_party/patches/musl/`, the per-arch `libc_*.ld` linker
  scripts.
- Remove musl-related plumbing from `cmake/user.cmake`
  (`PPAP_MUSL_*` variables, `_ppap_add_musl()`, the specs file
  generation).
- Delete or rewrite `tests/user/test_musl*` (these were libc smoke
  tests; the new equivalent tests PPAP libc instead).
- Update README, spec, target docs.

## Validation Plan

- **After M2.**  Every native applet's stripped binary should be
  within ±1% of its pre-M1 size.  `./scripts/run.sh --test qemu_arm`
  and `qemu_m68k` pass.
- **After M5.**  A new `tests/user/test_libc.c` self-test exercises
  every public function we added: `printf` width/zero-pad, `strtol`
  edge cases, `fopen`/`fread`/`fwrite` round-trip, `strftime`,
  `setjmp`/`longjmp`.
- **After M6.**  Rogue boots, displays the dungeon, accepts input,
  runs through one cycle of fight + descend without a crash.
- **After M7.**  `third_party/musl` is gone from `.gitmodules`,
  `find build/ -name 'musl*'` returns nothing, all tests still pass.

## Risks and Open Questions

- **`fscanf` complexity.**  Full `fscanf` is its own swamp.  Plan: do
  the smallest set of conversions Rogue needs, leave the rest unimplemented.
- **`malloc` quality.**  `uc_malloc` is a best-fit allocator over a
  fixed-size pool seeded by the app.  Standard `malloc` is expected to
  Just Work without a setup call.  Solution: have `crt0` seed a default
  heap pool before `main` runs (size configurable per-app via a weak
  symbol or default).
- **`errno` semantics shift.**  Today user apps interpret negative
  return as `-errno`.  Adopting the libc convention (`-1` + `errno`)
  for some wrappers and not others would be confusing.  Pick a single
  convention before M3 and stick with it; defer the syscall-wrapper
  rewrite if the conversion is large.
- **Floating-point in `printf`.**  Rogue likely doesn't need `%f`, but
  if it does, emitting it without pulling in soft-float helpers on the
  smaller targets needs care.  Inspect during M6.
- **ePIC RISC-V link path.**  musl currently provides the ePIC-aware
  CRT for RISC-V user binaries.  When musl goes, the existing
  freestanding crt0 and uclib must already work in ePIC mode (they do
  for native applets) — confirm Rogue is the only thing relying on
  musl's RISC-V CRT before deleting it.

## Out of Scope

- Replacing `push` with a different shell.  push stays.
- Adding pthreads or any threading primitive.
- Locale, wide chars, regex (the BRE in `sed.c` is enough for `sed`'s
  needs; `<regex.h>` is not exposed).
- Dynamic linking.  Everything stays statically linked / PIC.
