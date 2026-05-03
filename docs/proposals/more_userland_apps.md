# Proposal: More PPAP-Native Userland Apps

## Summary

PPAP shipped with a minimal native user-space and leaned on busybox
for ~25 utilities.  This proposal plans the gradual replacement of
busybox-supplied applets with native equivalents, written against
the bare-metal Path A toolchain (raw syscall stubs, no libc), layered
on a growing [src/user/lib/uclib](/src/user/lib/) of shared helpers.

The goal is not to eliminate busybox immediately — it stays as the
fallback for anything not yet reimplemented — but to shrink reliance
on it tier by tier.

Step 0 and Tiers 1–3 have landed (see git history).  Tiers 4 and 5
remain.

Normative references for implementers:

- [docs/user/userland_dev_guide.md](/docs/user/userland_dev_guide.md)
  — compiler flags, linking, memory layout, ELF loader contract.
- [src/user/README.md](/src/user/README.md) — directory layout and
  "Adding a New Program" checklist (crt0 + syscall.S link rules).
- [docs/getting_started/coding_rules.md](/docs/getting_started/coding_rules.md)
  — style, include-path, commit-message rules.

## Motivation

- **Size budgets.**  The pcxt target lives inside a single 64 KB
  segment for the kernel side and is tight on romfs space; the xtensa
  target is also memory-constrained.  A set of small, single-purpose
  native binaries is usually smaller than the equivalent slice of a
  musl-linked busybox multicall binary — *once enough applets are
  dropped from the busybox config to actually shrink it.*
- **Readability and debuggability.**  Native apps use the same syscall
  stubs, coding style, and `uclib` helpers as the rest of PPAP.  When
  something misbehaves we can step through it with `pdb` and find
  familiar code, instead of musl + busybox multicall plumbing.
- **Dependency reduction.**  Every applet we cover natively is one
  fewer reason we must keep building musl + busybox for new targets.
  This matters most for the i8086 / Xtensa ports, where neither is
  straightforward.

## Non-Goals

- **Replacing `hush`.**  `push` is the interactive shell; `hush`
  remains available via busybox for scripting compatibility until we
  decide whether to extend `push` or drop `hush` entirely.  That is a
  separate proposal.
- **Replacing Rogue or other full third-party apps.**  Only utilities
  covered by the busybox config are in scope.
- **POSIX-strict feature parity.**  Native applets implement the
  common-use subset.  Uncommon flags can be added on demand; the
  busybox fallback covers edge cases until then.
- **Per-architecture rewrites.**  All new applets are Path A native,
  portable across ARM / m68k / RISC-V / Xtensa / i8086 via the shared
  syscall ABI.  No target-specific forks.

## Current State (post-Tier 3)

### Native apps

Bootstrap and dev tools: `init`, `getty`, `push` (+ `push_line`),
`pdb`, `trace`, `hello`, `ttyctl` (pico1calc only).

TUI: `pi`, `pile`, `calc`, `top`.

Coreutils: `cat`, `ls`, `ps`, `df`, `uname`, `sleep`, `mkdir`,
`reset`, `rmdir`, `rm`, `kill`, `touch`, `date`, `cp`, `mv`, `chmod`,
`ln`, `wc`, `head`, `tail`, `printf`, `basename`, `dirname`, `yes`,
`cut`, `tr`.

Source: [src/user/](/src/user/).  Build list: `USER_APPS` in
[cmake/user.cmake](/cmake/user.cmake).

### push built-ins (no external binary needed)

`exit`, `true`, `false`, `cd`, `pwd`, `echo`, `export`, `unset`,
`set`, `env`, `.` / `source`, `break`, `continue`, `shift`,
`history`.  Source: [src/user/push.c](/src/user/push.c)
`is_builtin()`.

### Remaining busybox applets

Text: `grep`, `sort`, `sed`.
File ops: `mount`, `umount`.
Shell: `hush` / `sh` — out of scope (see Non-Goals).

### uclib surface

Output: `uc_puts`, `uc_eputs`, `uc_putc`, `uc_putu`, `uc_puti`,
`uc_putx{8,16,32}`, `uc_snprintf`, `uc_vsnprintf`, `uc_printf`,
`uc_eprintf`.
Strings/memory: `uc_strlen`, `uc_strcmp`, `uc_strncmp`, `uc_strcpy`,
`uc_strncpy`, `uc_strchr`, `uc_strrchr`, `uc_memcpy`, `uc_memset`,
`uc_memcmp`.
Numeric: `uc_atoi`, `uc_parse_u32`.
Path: `uc_basename`.
File: `uc_copy_fd`.
Calendar: `uc_gmtime`, `uc_format_ymdhm`.
Environment: `environ`, `uc_getenv`, `_uclib_init_env`.
Heap: `uc_heap_init`, `uc_malloc`, `uc_free`
(see [docs/user/uc_malloc.md](/docs/user/uc_malloc.md)).

## Plan

Replacement is organised in tiers.  Within a tier, applets can be
implemented in any order, but the whole tier should land (and the
corresponding `CONFIG_*=y` entries be removed from busybox) before
moving on.  Each tier cut from busybox actually shrinks the binary,
avoiding the "naive replacement temporarily increases size" trap.

### Tier 4 — Heavy text utilities (complete)

~~`grep`~~ (fixed-string match) and ~~`sort`~~ (in-memory) both landed.
Tier 4 done; the only busybox text utility left is `sed`.

Deferred: `sed`, regex support for `grep` (BRE/ERE), external-merge
sort for >RAM input.  Full `sed` is substantial and busybox covers it
well.  Same for grep regex — fixed-string covers the common case.
Sort merge would require a temp-file phase that we don't need yet.

What landed:
- `grep`: fixed-string match with `-n -i -v -c -q -h -H -F`, multi-file
  + stdin, 1 KB line buffer (longer lines silently truncated for
  matching).  ~7 KB stripped per arch.
- `sort`: `-r -n -u -f`, multi-file + stdin, in-memory only.
  4 KB `uc_malloc` heap pool, growing input buffer + `line_t` index.
  Insertion sort (small-N inputs only).  ~6.5 KB stripped per arch.

### Tier 5 — System and admin

Applets: `mount`, `umount`, `free`.

Characteristics:
- `free` reads `/proc/meminfo` (already exposes MemTotal / MemFree /
  PageSize / DataMax / OomCount) — pure user-space.
- `mount` / `umount`: listing mounts is a `/proc/mounts` read, but
  the act of mounting / unmounting needs new user-space wrappers for
  `SYS_MOUNT` (0x0900) and `SYS_UMOUNT2` (0x0901) added across all
  five arch syscall.S files (the kernel side already exists).
- `date` is already native (landed with Tier 2); not in this tier.

Dropped: `dmesg`.  PPAP has no klog ring buffer today and adding one
costs RAM on every target — for a feature whose live use case (boot
diagnostics) is already covered by the immediate UART / console
output during boot.  Reintroduce only if a concrete need for
post-hoc log inspection emerges.

## Open follow-ups (kernel / uclib gaps surfaced by earlier tiers)

- **SIGPIPE / -EPIPE on write to closed pipe.**  Surfaced by
  `yes | head -n N`: when head exits, yes blocks indefinitely in
  `write()` instead of receiving SIGPIPE or -EPIPE.  Affects every
  infinite-producer pipeline.  `tests/user/test_pipe.c` covers the
  EOF-on-read case but not the EPIPE-on-write case.  Filing as a
  kernel issue.
- **`tail` stdin / non-seekable input.**  Currently file-only because
  the implementation walks backward via `lseek`.  Pipe support needs
  either a `uc_getline` helper or a `uc_malloc` ring buffer.
- **`uc_getline`.**  Was anticipated in the original Tier 3 plan but
  not actually needed — every Tier 3 applet got by with a chunked
  read loop or `lseek`.  Add when a future applet genuinely benefits.
- **push pipe builtin handling.**  `echo foo | wc` reports
  `push: echo: not found` because push exec's external commands for
  pipe stages instead of running its `echo` builtin.  Test scripts
  must use `printf` (the native applet) or single-quoted `cat << EOF`
  redirection.  Worth either fixing in push or documenting clearly.
- **`ls` colorizes when stdout is a pipe.**  `ls /bin | sort` produces
  visually-unsorted output because every symlink entry is wrapped in
  `ESC[1;36m` … `ESC[0m`, and `sort` (correctly) sorts by raw bytes —
  ESC sorts before all letters, so all colored entries float to the
  top.  POSIX `ls` only colorizes when stdout is a TTY; PPAP's `ls`
  should follow that convention (test with `isatty` / fallback flag).
  Surfaced while smoke-testing native `sort`.

## Output style: colorful VT100 by default

PPAP's native userland is POSIX-*flavoured*, not POSIX-strict.  One
deliberate divergence is output style: apps prefer colored, VT100 /
ANSI-escape output by default, turned off on request.  Existing apps
(`ls`, `ps`, `df`, `top`, `hello`, `push_line`, `ttyctl`) all follow
the same idiom — new applets should match it:

```c
static int use_color = 1;
#define C(seq) (use_color ? (seq) : "")

#define C_RST     C("\033[0m")
#define C_BOLD    C("\033[1m")
#define C_RED     C("\033[31m")
#define C_GREEN   C("\033[32m")
/* ... add only the colors this applet uses ... */
```

Rules:

- Each applet defines its own `C_*` macro block containing only the
  sequences it actually uses — no shared palette header.  The
  indirection through `C(...)` is what keeps the strings
  flash-resident on ARM XIP without any extra linker-script work.
- Every applet that emits color accepts a `--no-color` long option
  that sets `use_color = 0`.  Matching an environment variable
  (`NO_COLOR`) is a nice-to-have via `uc_getenv()` (see
  [uclib.h](/src/user/lib/uclib.h)) but not required.
- Escape sequences that degrade harmlessly on reduced-capability
  targets (e.g. `\033[2m` dim on PicoCalc VGA attr mapping) are
  acceptable — comment them as such, as `top.c` does.
- Do not colorize stderr-only diagnostics unless the information is
  genuinely easier to read with color (errors red, warnings yellow is
  fine; coloring every error prefix is noise).
- Keep the uncolored form readable.  Colors are decoration, not
  information carriers.  Any semantic channel must also be encoded
  in the text (e.g. `ls` appends `/` for directories in addition to
  coloring them blue).

See the existing implementations for reference:
[src/user/ls.c](/src/user/ls.c),
[src/user/ps.c](/src/user/ps.c),
[src/user/df.c](/src/user/df.c),
[src/user/top.c](/src/user/top.c),
[src/user/hello.c](/src/user/hello.c).

## Rollout discipline per applet

For each applet, follow the "Adding a New Program" recipe in
[src/user/README.md](/src/user/README.md) and the developer guide's
build/link rules, then:

1. Create `src/user/<app>.c`.  Respect
   [coding_rules.md](/docs/getting_started/coding_rules.md):
   80-column, snake_case, no target `#ifdef` in shared user code.
   No `#include <stdio.h>` etc. — use
   [src/user/syscall.h](/src/user/syscall.h) and
   [src/user/lib/uclib.h](/src/user/lib/uclib.h) only.
2. Add to `USER_APPS` in
   [cmake/user.cmake](/cmake/user.cmake) (and the ttyctl-style
   conditional list if it's platform-specific).  For pcxt, the
   parallel install pipeline also needs entries in
   [`src/target/pcxt/CMakeLists.txt::PCXT_USER_APPS`](/src/target/pcxt/CMakeLists.txt)
   and [`scripts/mkpcimg.sh::USER_APPS`](/scripts/mkpcimg.sh) — three
   lists in lockstep.
3. If the applet replaces a busybox-shipped utility, **also** remove
   the corresponding entry from `BB_APPLETS` in `cmake/user.cmake`
   (otherwise the busybox `/bin/<app>` symlink shadows the native
   binary in romfs) **and** drop the `CONFIG_<APP>=y` from
   [busybox_ppap.fragment](/third_party/patches/busybox/busybox_ppap.fragment).
   Three lists in lockstep here too.
4. For thin syscall wrappers, dedicated `tests/user/test_<app>.c`
   is not required — the underlying syscalls already have direct
   coverage in `test_fs`, `test_tmpfs`, etc., and vfork/exec plumbing
   is not what needs re-testing.  Extend an existing test file if the
   applet exercises a previously uncovered kernel path.
5. Verify romfs size on pcxt (tightest budget) and xtensa; smoke-test
   in QEMU shell on ARM and m68k.

## Open Questions

- **Multicall or not.**  All landed Tier-1/2/3 applets came in at
  5–10 KB stripped (see romfs).  Multicall isn't compelling at this
  scale; stay with individual ELFs unless Tier 4 (`grep`, `sort`)
  tips the trade-off.
- **When to drop busybox entirely.**  Once Tier 4 lands the only
  remaining busybox applets are `sed`, `mount`, `umount`, and `hush`.
  Decide whether the multi-MB musl + busybox build still earns its
  place at that milestone.
- **PicoCalc-only utilities.**  Some future apps (frame buffer
  demos, keyboard test) will be target-specific.  Those live under
  `src/target/<target>/user/` rather than `src/user/` and are out of
  scope here.

## Verification

Each tier's rollout commits include native-shell smoke tests on
qemu_arm and qemu_m68k against host tool output.  Romfs size is
measured at the `romfs_<target>.bin` artifact, compared against the
pre-Step-0 baseline (kept in git history if a delta is needed).
