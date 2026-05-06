# Proposal: Promote `uclib` to a Minimal PPAP libc, Drop musl

## Status

- **M1 — header scaffold**, `6a47e7a`. POSIX-named header tree under
  `src/user/include/`, no source change, byte-identical binaries.
- **M2 — rename `uc_*` to standard names**, `5017e7c`. `uclib.c`
  split into per-header source files.
- **M3 — POSIX gap fill**, `ca68485`. `<ctype.h>`, plus `strtol /
  strtoul / abs / labs / qsort / bsearch / strstr / strspn / strcspn
  / strpbrk / strdup`.
- **M4 — FILE streams**, `8faaff8`. `lib/file.c`; the bulk of
  `uc_*` stdio shortcuts retire.
- **M5 — signal / time / setjmp**, `0ce50b0`.  Per-arch
  `signal()` + `setjmp/longjmp`; `<time.h>` with `gmtime / localtime
  / mktime / strftime`.  `tests/user/test_libc.c` self-test (55
  assertions, was 42 in M5 then 55 after M6a).
- **M6a — libc gaps surfaced by Rogue**, `9ca1570`. `errno` storage,
  `raise / abort / calloc / strerror / perror / puts / sprintf /
  sscanf / setbuf / strcat / strncat / rand / srand`,
  `__errno_location`.
- **M6b — Rogue links against PPAP libc**, `a2a010f`. `build_rogue.sh`
  rewritten; the rogue ELF starts and prints its banner on
  qemu_arm.  Surfaced extra POSIX surface: `<sys/types.h>`,
  `<sys/ioctl.h>`, `<arpa/inet.h>`, `<limits.h>`, `tcgetattr /
  tcsetattr`, `sleep / fork / wait / getuid / getgid / execl /
  getpass / lstat`, `STDIN_FILENO` / `STDOUT_FILENO` /
  `STDERR_FILENO`, `<sys/stat.h>` declarations.
- **M6c — rogue gameplay verification**, *pending*.  Banner appears;
  full curses gameplay needs an interactive terminal session — not
  yet captured.
- **M7 — drop musl**, *in progress* (current change).  Submodule and
  build script gone; `tests/user/test_musl*` removed; `_ppap_add_musl()`,
  `ppap_musl_test_program()`, `PPAP_MUSL_*` plumbing dropped from
  `cmake/user.cmake`; the qemu_rv32 UFS staging that existed solely
  to ship oversized musl-linked test binaries is removed; comment
  cleanup pending in target docs / README.

## Summary

PPAP linked against musl libc to support exactly one consumer:
Rogue 5.4.4.  Every other user-space program — the ~50 native
applets and the push shell — used `uclib`, a small freestanding
helper library with `uc_*` prefixed symbols and PPAP-internal
headers.

This proposal grew `uclib` into a real (but minimal) libc, exposed
under the standard POSIX header names, and migrated Rogue onto it so
musl could be removed entirely.

The aim was **not** a fully POSIX-conformant libc.  It is the
smallest libc that lets PPAP's userland and Rogue compile and run,
written in freestanding C and matched to the kernel's syscall
surface.

Outcome: a single, in-tree libc with no submodule, no per-arch musl
patches, no separate sysroot — and a userland that uses `<stdio.h>`,
`<string.h>`, `<stdlib.h>` like any other C program.

## Motivation

Reasons to do this:

- **Build / setup cost.**  musl was a submodule, had a custom per-arch
  build script (`third_party/build_musl.sh`), and produced a sysroot
  of ~1 MB per architecture.  ePIC RISC-V additionally carried a
  patched CRT and a custom linker script.  All of that existed to
  support one third-party app.
- **Toolchain simplicity.**  Removing musl means each user program is
  compiled the same way (freestanding, `-nostdlib`, in-tree headers).
  The "musl-linked vs uclib-linked" split disappears.
- **Coherence.**  Before the work the native applets called
  `uc_strlen`, `uc_printf`, etc., while ports used `strlen`, `printf`.
  Two parallel idioms for the same functions.  One libc with standard
  names removes that split.
- **Footprint control.**  A purpose-built libc lets us decide what to
  ship and what to skip (no locale, no wide-char, no NSS, no dlfcn).
  musl is small but it is not microcontroller-small.

Trade-offs:

- **Effort.**  Writing libc functions is unglamorous, and the FILE
  stream layer is non-trivial.  Realised cost: ~3000 net lines added
  to PPAP across M1–M6b; M7 removes ~10000+ lines of musl plus its
  build infrastructure.
- **Compatibility tax.**  Future ports of unmodified third-party apps
  may pull in a libc surface we have not implemented.  We either add
  the missing piece or patch the port.

## Goals

- Provide POSIX-named headers (`<stdio.h>`, `<string.h>`, `<stdlib.h>`,
  `<unistd.h>`, `<errno.h>`, `<signal.h>`, `<time.h>`, `<ctype.h>`,
  `<setjmp.h>`, `<limits.h>`, `<termios.h>`, plus `<sys/stat.h>`,
  `<sys/wait.h>`, `<sys/types.h>`, `<sys/ioctl.h>`, `<sys/uio.h>`,
  `<sys/utsname.h>`, `<sys/mount.h>`, `<sys/ptrace.h>`, `<sys/statfs.h>`,
  `<arpa/inet.h>`).
- Migrate every native applet to standard names; drop the `uc_*`
  prefix.
- Build Rogue against the new libc with no patches beyond the
  existing curses shim.
- Remove the musl submodule, build script, sysroot, linker scripts,
  and related test machinery once Rogue is migrated.

## Non-Goals

- A POSIX-conformant libc.  We are matching what PPAP's userland and
  Rogue actually use, not a spec.
- Locale support, wide-char (`wchar_t` / `<wchar.h>`), `iconv`, NSS,
  dynamic linking, regular `<regex.h>` (sed has its own BRE engine).
- Threading.  PPAP user space is single-threaded; pthreads stays out.
- Floating-point printf precision matching glibc.

## Header Layout (as shipped)

User programs are compiled with a system include path:

```
-isystem src/user/include
```

`src/user/include/` holds POSIX-named headers.  Some are forwarding
shims into `src/common/` (which already defines the kernel-shared
ABI types like `struct stat`, `struct dirent`); others are written
from scratch.

**No symlinks.**  Each forwarding header is a real file that
`#include`s the matching `src/common` header.  This keeps the
inter-module dependency convention explicit (user is allowed to
depend on common; the forwarding direction is recorded in the
header itself).

### Forwarding headers (delegate to `src/common/`)

| POSIX header        | Forwards to              |
| ------------------- | ------------------------ |
| `<dirent.h>`        | `common/dirent.h`        |
| `<errno.h>`         | `common/errno.h`         |
| `<fcntl.h>`         | `common/fcntl.h`         |
| `<poll.h>`          | `common/poll.h`          |
| `<signal.h>`        | `common/signal.h`        |
| `<termios.h>`       | `common/termios.h`       |
| `<time.h>`          | `common/time.h`          |
| `<sys/stat.h>`      | `common/stat.h`          |
| `<sys/statfs.h>`    | `common/statfs.h`        |
| `<sys/uio.h>`       | `common/iovec.h`         |
| `<sys/wait.h>`      | `common/wait.h`          |
| `<sys/utsname.h>`   | `common/utsname.h`       |
| `<sys/mount.h>`     | `common/mount.h`         |
| `<sys/ptrace.h>`    | `common/ptrace.h`        |
| `<sys/ioctl.h>`     | `common/termios.h` (for `struct winsize` + `ioctl()` decl) |

### Stand-alone headers

`<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<ctype.h>`, `<unistd.h>`,
`<setjmp.h>`, `<sys/types.h>`, `<arpa/inet.h>`, `<limits.h>`.

`<unistd.h>` exposes the syscall wrappers PPAP provides
(`read`, `write`, `open`, `close`, `lseek`, `getpid`, `fork`,
`execve`, `sleep`, `getuid` / `getgid`, …).

### Implementation files

Final layout under `src/user/lib/`:

```
string.c   strlen, strcmp, strncmp, strcpy, strncpy, strchr, strrchr,
           strstr, strspn, strcspn, strpbrk, strdup, strcat, strncat,
           strerror, memcpy, memmove, memset, memcmp
stdio.c    putchar, puts, printf, fprintf, vfprintf, snprintf,
           vsnprintf, sprintf, vsprintf, sscanf, vsscanf, perror,
           setbuf
stdlib.c   atoi, strtol, strtoul, abs, labs, qsort, bsearch, calloc,
           abort, exit, rand, srand, getenv, environ, _uclib_init_env
alloc.c    malloc / free / uc_heap_init  (separate TU so the host
           unit test can link just this file)
file.c     FILE streams: stdin / stdout / stderr, fopen, fclose,
           fflush, fread, fwrite, fputs, fputc, fgetc, getc, getchar,
           fgets, ungetc, feof, ferror, clearerr, fseek, ftell,
           rewind, setvbuf
time.c     time, gmtime[_r], localtime[_r], mktime, strftime
errno.c    int errno; + __errno_location()
signal.c   raise()
util.c     POSIX shims (sleep, fork, wait, getuid / geteuid /
           getgid / getegid, execl, getpass, tcgetattr / tcsetattr)
           plus the uc_-prefixed survivors (uc_copy_fd, uc_parse_u32)
```

Per-arch:

```
src/arch/<arch>/user/sigaction.c    signal()  (calls rt_sigaction
                                     with the arch's sigreturn
                                     trampoline.  arm_m / m68k /
                                     riscv only.)
src/arch/<arch>/user/setjmp.S       setjmp / longjmp  (arm_m, m68k,
                                     riscv only — xtensa and i16
                                     skipped; apps using <setjmp.h>
                                     do not link there.)
```

## Resolved milestones

Milestone numbering is the recommended order; M3 / M4 / M5 are
independent and could have been done in any order after M2.

### M1 — Header reorg, behavior unchanged ✅

Added `src/user/include/` with forwarding and skeleton headers.
Added `-isystem src/user/include` to user-program compile flags.
No source change; per-app stripped binaries byte-identical to
pre-M1.

### M2 — Rename `uc_*` to standard names ✅

Mechanical sweep across `src/user/*.c` and `src/user/lib/*`.  Split
`uclib.c` into `string.c`, `stdio.c`, `stdlib.c`, …  Header guards
updated from `PPAP_USER_LIB_*` to `_STDIO_H`-style.

Functions whose semantics matched POSIX got the standard name; the
ones whose semantics differed (`uc_puts` / `uc_eputs` writing
without auto-newline, etc.) were kept as `uc_`-prefixed TODO markers
until M4 brought FILE streams.

### M3 — Standard library gaps (lightweight) ✅

Added `strtol`, `strtoul`, `qsort`, `bsearch`, `abs`, `labs`,
`memmove`, `strstr`, `strspn`, `strcspn`, `strpbrk`, `strdup`, plus
the full `<ctype.h>` (header-only inlines).

Decision recorded at the time: the `errno` variable was **not**
added in M3 — adding storage with no setter would only hide bugs.
It eventually landed in M6a together with the syscall-wrapper
constants.

### M4 — `<stdio.h>` FILE streams ✅

Added `lib/file.c` with the FILE struct, `stdin`/`stdout`/`stderr`,
fopen/fclose/fflush/fread/fwrite/fputs/fputc/fgetc/getchar/fgets/
ungetc/feof/ferror/clearerr/fseek/ftell/rewind/setvbuf.  `printf`
became `vfprintf(stdout, …)` so caller-installed buffering works.
The `uc_puts` / `uc_eputs` / `uc_eprintf` / `uc_putu` / `uc_puti`
/ `uc_putx{8,16,32}` shortcuts retired during the migration sweep
(~600 callers updated to `fputs(s, stdout)` / `fputs(s, stderr)`
/ `fprintf(stderr, …)` / `printf(...)`.)

Default stream policy: `stdin` / `stdout` / `stderr` are unbuffered
(matches pre-M4 behaviour, no malloc); `fopen`-d streams are fully
buffered with a 512 B malloc'd buffer.

### M5 — `<signal.h>`, `<time.h>`, `<setjmp.h>` ✅

`signal()` per-arch (arm_m / m68k / riscv); xtensa and i16 stubs
renamed `sigaction` → `signal` (the existing 2-arg syscall already
matches signal()'s shape).  `<time.h>` got `time_t`, `struct tm`,
`gmtime[_r]`, `localtime[_r]`, `mktime`, `strftime`.  Per-arch
`setjmp.S` on arm_m / m68k / riscv.

`tests/user/test_libc.c` introduced with 42 assertions covering
printf width/zero-pad, strtol/strtoul, ctype, qsort/bsearch, FILE
round-trip on /tmp, strftime, setjmp/longjmp.

### M6a — libc gaps surfaced by Rogue ✅

Survey of the rogue.elf undefined-symbol set against the existing
libc revealed the missing entries.  Added `int errno` + `int *
__errno_location(void)`, `raise()`, `abort()`, `calloc()`,
`strerror()`, `perror()`, POSIX `puts()` (with auto-newline),
`sprintf` / `vsprintf`, `sscanf` / `vsscanf` (subset),
`strcat / strncat`, `setbuf`, `rand / srand`.  test_libc.c grew to
55 assertions.

### M6b — Rogue links against PPAP libc ✅

`build_rogue.sh` was rewritten to drop musl entirely: it now uses
`-isystem src/user/include` (plus the rogue patches dir) for
headers, and links against the same pre-built CRT + libc objects
under `$PPAP_SHARED_BUILD` that the regular `ppap_user_program()`
pipeline produces, plus libgcc.

The build itself surfaced more libc surface that wasn't strictly
visible in the symbol-name survey:

- POSIX shims over existing kernel-side syscalls: `sleep / fork /
  wait / getuid / geteuid / getgid / getegid / execl / getpass`,
  `tcgetattr / tcsetattr`, `lstat`.
- New headers needed by rogue's includes: `<sys/types.h>` with
  `pid_t / ssize_t / off_t / mode_t / uid_t / gid_t / time_t / …`,
  `<sys/ioctl.h>`, `<arpa/inet.h>` with `htonl / ntohl / htons /
  ntohs`, a self-contained `<limits.h>` (no `#include_next` chain —
  it broke on the ePIC clang RV32 toolchain), `STDIN_FILENO` /
  `STDOUT_FILENO` / `STDERR_FILENO` macros in `<unistd.h>`.
- `<sys/stat.h>` started declaring `stat / lstat / mkdir / chmod`
  rather than only forwarding the type / mode macros.
- common/termios.h gained `IEXTEN`, `TCSANOW` / `TCSADRAIN` /
  `TCSAFLUSH`, `VMIN`, `VTIME`, plus per-arch syscall.S got an
  `lstat` stub (SYS_LSTAT64=0x0304).

Rogue prints its "Hello nobody, just a moment while I dig the
dungeon..." banner on qemu_arm.  Three-arch build verified:
qemu_arm, qemu_m68k, qemu_rv32 all produce a stripped rogue ELF.

## Pending milestones

### M6c — Rogue gameplay verification

The banner appears; what we don't yet have is an automated proof
that rogue plays through to a death-or-quit cycle.  Curses output
is hard to capture via piped stdin smoke tests, so this is left as
a manual play test — load qemu_arm in a tty, type `rogue`, navigate
the dungeon, exit via `Q`.  Defects found here would be fed back
into M6a / M6b.

### M7 — Drop musl

In progress (current change).

- Removed `third_party/musl` submodule (deinit, dropped from
  `.gitmodules`, removed from index).
- Deleted `third_party/build_musl.sh`,
  `third_party/patches/musl/` (linker scripts + ePIC overlay).
- Removed musl-related plumbing from `cmake/user.cmake`
  (`PPAP_MUSL_*` variables, `_ppap_add_musl()`,
  `ppap_musl_test_program()`, the specs file generation,
  `USER_MUSL_TESTS`).
- Deleted `tests/user/test_musl*.c` (5 files); their entries
  removed from runtests.c and runtests_ext.c.
- `src/target/qemu_rv32/CMakeLists.txt` no longer assembles a
  separate UFS image to ship oversized musl-linked tests; the
  `__ufsimg_*` symbols and ufsimg.h header go away.
- Pending: docs sweep (README, spec_v07.md, target docs) for
  remaining "musl" mentions that are now historical; the comment
  in `src/common/*.h` saying "Layout matches Linux ARM (used by
  musl libc)" can lose the parenthetical.

## Validation

Test counts at each milestone:

| After | qemu_arm | qemu_m68k | qemu_rv32 | pcxt   | host |
| ----- | -------- | --------- | --------- | ------ | ---- |
| M1    | 24/24    | 24/24     | 18/18     | 18/18  | 8/8  |
| M2    | 24/24    | 24/24     | 18/18     | 18/18  | 8/8  |
| M3    | 24/24    | 24/24     | 18/18     | 18/18  | 8/8  |
| M4    | 24/24    | 24/24     | 18/18     | 18/18  | 8/8  |
| M5    | 25/25    | 25/25     | 19/19     | 18/18 \* | 8/8  |
| M6a   | 25/25    | 25/25     | 19/19     | 18/18 \* | 8/8  |
| M6b   | 25/25    | 25/25     | 19/19     | 18/18 \* | 12/12 |

\* test_libc skipped on pcxt: i16 has no setjmp.S.

Plus: rogue prints its startup banner on qemu_arm under M6b.

## Resolved questions and lessons

- **`fscanf` complexity.**  Settled: the subset that Rogue (and the
  PPAP `sscanf` callers) actually need is small enough to hand-roll
  (`%d`, `%u`, `%x`, `%s`, `%c`, `%%`, optional width on `%s`/`%c`).
  Anything heavier is left unimplemented.
- **`malloc` quality.**  Unresolved.  `uc_heap_init` still has to
  be called by each app before the first `malloc`.  `crt0` could
  seed a default pool but rogue already calls heap-init equivalents
  during its own startup, so we have not pushed this further yet.
  Filed as a follow-up.
- **`errno` semantics shift.**  Unresolved at the syscall level.
  The kernel-side syscall stubs still return negative error codes
  directly; the `errno` global exists but no wrapper currently sets
  it.  Rogue's call sites that read `errno` typically only hit the
  paths that `perror()` / `strerror()` cover.  The full conversion
  is filed as a follow-up.
- **Floating-point in `printf`.**  Did not surface — Rogue's
  formatted output in our build does not request `%f` (the curses
  shim translates everything through the basic `%s` / `%d` / `%u`
  conversions).
- **ePIC RISC-V link path.**  Resolved.  PPAP's user-side ePIC
  toolchain works with the freestanding crt0 + the new PPAP libc
  objects without any musl-specific overlay; rogue links cleanly on
  qemu_rv32.

## Out of scope

- Replacing `push` with a different shell.  push stays.
- Adding pthreads or any threading primitive.
- Locale, wide chars, regex (the BRE in `sed.c` is enough for
  `sed`'s needs; `<regex.h>` is not exposed).
- Dynamic linking.  Everything stays statically linked / PIC.
