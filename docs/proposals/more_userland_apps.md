# Proposal: More PPAP-Native Userland Apps

## Summary

PPAP currently ships a minimal set of native user-space programs
(`init`, `getty`, `push`, `cat`, `ls`, `ps`, `df`, `top`, `pi`, `pdb`,
`trace`, `ttyctl`, `hello`) and leans on busybox for the remaining
~25 common utilities.  This proposal plans the gradual replacement of
busybox-supplied applets with PPAP-native equivalents, written against
the bare-metal Path A toolchain (raw syscall stubs, no libc), and
layered on a growing [src/user/lib/uclib](/src/user/lib/) of shared
helpers.

The goal is not to eliminate busybox immediately — it stays as the
fallback for anything we have not reimplemented — but to shrink our
reliance on it tier by tier, and to have a coherent plan for the order
in which we do so.

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
- **No orphan duplication.**  We already have native `cat`, `ls`,
  `ps`, `df`, `top` shadowing busybox entries.  The current state is
  inconsistent (some replaced, some not) — this proposal makes the
  direction explicit.

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

## Current State

### Native apps (20 + 1 target-specific)

`init`, `getty`, `push` (+ `push_line`), `cat`, `ls`, `ps`, `df`,
`top`, `pi`, `pdb`, `trace`, `hello`, plus the Tier 1 additions:
`uname`, `sleep`, `mkdir`, `reset`, `rmdir`, `rm`, `kill`.
`ttyctl` is pico1calc-only.
Source: [src/user/](/src/user/).  Build list: `USER_APPS` in
[cmake/user.cmake](/cmake/user.cmake).

### push built-ins (no external binary needed)

`exit`, `true`, `false`, `cd`, `pwd`, `echo`, `export`, `unset`,
`set`, `env`, `.` / `source`, `break`, `continue`, `shift`, `history`.
Source: [src/user/push.c](/src/user/push.c) `is_builtin()`.

### busybox applets (remaining, from
[busybox_ppap.fragment](/third_party/patches/busybox/busybox_ppap.fragment))

File ops: `cp`, `mv`, `ln`, `chmod`, `mount`, `umount`.
Text: `grep`, `head`, `tail`, `wc`, `sort`, `sed`, `printf`.
Shell: `hush` / `sh` — out of scope (see Non-Goals).

Shadowed-by-native applets (`ls`, `cat`, `ps`, `df`, `echo`) and the
Tier 1 set (`uname`, `sleep`, `mkdir`, `rmdir`, `rm`, `kill`) have
been removed from the fragment — see Step 0 / Tier 1 status below.

## Plan

Replacement is organised in tiers of rising complexity.  Each tier is
a separate rollout; within a tier, applets can be implemented in any
order, but the whole tier should land (and the corresponding
`CONFIG_*=y` entries be removed from busybox) before moving on.  This
avoids the "naive replacement temporarily increases size" trap,
because each tier cut from busybox actually shrinks the binary.

### Step 0 — Preliminary cleanup (landed)

- Drop `CONFIG_LS=y`, `CONFIG_CAT=y`, `CONFIG_PS=y`, `CONFIG_DF=y`,
  `CONFIG_ECHO=y` from
  [busybox_ppap.fragment](/third_party/patches/busybox/busybox_ppap.fragment)
  — already covered by native apps or push built-ins.
- Follow-up: also drop `echo` from `BB_APPLETS` in
  [cmake/user.cmake](/cmake/user.cmake) so the `/bin/echo → busybox`
  symlink is not created in romfs.

Commits: `f798f61`, `7d3d0e2` (echo BB_APPLETS fixup).
Initial busybox shrink: qemu_arm 185,528 → 161,508 B (−24 KB, −13%);
qemu_m68k 259,120 → 227,780 B (−31 KB, −12%).

### Tier 1 — Trivial syscall wrappers (landed)

Applets landed: `uname`, `sleep`, `mkdir`, `reset`, `rmdir`, `rm`,
`kill`.

Deferred: `sync` — PPAP has no `SYS_SYNC` and no writeback cache to
flush.  Add the applet only if a future kernel change introduces
one.

Commits:

- `5905cfb` — uname (with SYS_UNAME renumbering to 0x0B01)
- `f54ba5b` — sleep (promoted `struct timespec` to `common/time.h`)
- `6dc5ab1` — mkdir
- `52ad3fe` — mkdir -p fix for mount-boundary EROFS
- `dd14a40` — reset (net-new applet)
- `bf7bd21` — rmdir
- `a324271` — rm (with `-r` recursion)
- `e18c65d` — kill

`reset` has no busybox counterpart in the current fragment — it's a
genuinely new applet, not a replacement.  It's in this tier because
PPAP subsystems (Human68k, MS-DOS, CP/M, S-OS) rewrite termios/VT100
state to match their guest OS, and crashes leave the host tty
confused.  `reset` restores POSIX defaults so an interactive shell
is usable again.

Supporting kernel changes that came out of Tier 1 work:

- `55f25df` — vfs: resolve relative paths in `vfs_lookup_parent`
  (mkdir/rmdir/unlink/rename/creat now accept relative paths, matching
  vfs_lookup_flags' existing behaviour).
- `f0ff7c0` — tests: cover relative paths for file-creation syscalls
  (test_tmpfs grew from 28 to 37 asserts).
- `5b6eb78` — user: `ls -R` recursive listing, a small
  dogfood-the-new-applets improvement.

Cumulative busybox shrink through Tier 1:

  qemu_arm  : 185,528 → 158,916 B  (−26,612 B, −14%)
  qemu_m68k : 259,120 → 224,620 B  (−34,500 B, −13%)

No shared `uclib` helpers were introduced — the existing
`uc_puts`/`uc_eputs`/`uc_atoi`/`uc_snprintf`/`uc_strcmp`/
`uc_parse_u32` surface covered Tier 1.  Future tiers may add
`uc_perror_errno` / `uc_copy_fd` / `uc_getline` / `uc_vsnprintf` as
needed.

### Tier 2 — File operations

Applets: `cp`, `mv`, `ln`, `chmod`, `touch`.

Characteristics:
- Need read-to-EOF / write-loop plumbing.  Shared in uclib.
- `cp` and `mv` need directory traversal for `-r` / recursive moves;
  start with single-file mode and defer `-r` to a follow-up.
- `ln` is hard-link only until VFS grows symlink support in more
  places; soft links stay busybox-backed (or noted).

Shared helpers introduced: `uclib_copy_fd`, minimal `getopt`-lite,
`uclib_fprintf` (buffered stderr).

### Tier 3 — Simple text utilities

Applets: `head`, `tail`, `wc`, `printf`, `tr`, `cut`, `yes`,
`basename`, `dirname`.

Characteristics:
- Line-oriented processing on top of uclib buffered I/O.
- `printf` implies a proper format-string parser — promote the
  minimal `uclib_printf` we will have written by this point into a
  real `uclib_vsnprintf`.

Shared helpers introduced: `uclib_getline`, `uclib_vsnprintf`.

### Tier 4 — Heavy text utilities

Applets: `grep` (fixed string first, then basic regex), `sort` (line
buffer + merge for >RAM input).

Deferred: `sed`.  A full `sed` implementation is substantial and
busybox covers it well; only reimplement if we have a specific need.

Characteristics:
- `grep` and `sort` are where native apps start to push against the
  128 KB data budget.  Streaming / chunked algorithms required.

### Tier 5 — System and admin

Applets: `mount`, `umount`, `dmesg`, `date`, `free`.

Characteristics:
- `mount` / `umount` must match whatever syscall surface the VFS
  actually exposes today — verify before implementing.  Likely a
  thin wrapper.
- `dmesg`, `free` read from procfs.
- `date` needs clock_gettime + the calendar-conversion helpers we
  already have for `ls -l` / `ps`.

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
  [uclib.h](/src/user/lib/uclib.h)) but not required in the initial
  tiers.
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
3. For thin syscall wrappers, dedicated `tests/user/test_<app>.c`
   is not required — the underlying syscalls already have direct
   coverage in `test_fs`, `test_tmpfs`, etc., and vfork/exec plumbing
   is not what needs re-testing.  Extend an existing test file if the
   applet exercises a previously uncovered kernel path (e.g. the
   relative-path regression added to `test_tmpfs` alongside Tier 1's
   `vfs_lookup_parent` fix).
4. Remove the corresponding `CONFIG_<APP>=y` from
   [busybox_ppap.fragment](/third_party/patches/busybox/busybox_ppap.fragment)
   once the native binary is in place.
5. Verify romfs size on pcxt (tightest budget) and xtensa; rerun
   `./scripts/run.sh --test` on ARM and m68k.

## Size budget

Actual Tier 1 per-applet sizes (stripped, in the romfs):

| Applet | qemu_arm | qemu_m68k | pcxt (.elf) |
|---|---|---|---|
| `uname` | 23,600 | 27,052 | 7,520 |
| `sleep` | 21,440 | 26,200 | 7,240 |
| `mkdir` | 23,180 | 27,252 | 7,616 |
| `reset` | 21,256 | 25,352 | 6,996 |
| `rmdir` | 22,204 | 26,328 | 7,252 |
| `rm` | 24,956 | 28,680 | 7,844 |
| `kill` | 27,296 | 31,256 | 9,156 |

The ARM/m68k numbers include ELF overhead that the ia16 flat-binary
loader strips, which is why pcxt is ~3× smaller per applet.  Tier 1
shows that the initial size estimate (200–600 B) was way off; even
a "trivial" applet drags in ~20 KB of `uclib` + crt0 + syscall stubs
on ARM/m68k.  Expect Tier 2+ applets to grow less proportionally
once the shared surface is amortised.

Shared `uclib` code is linked once per binary (bare-metal, no
multicall) — this is the main trade-off vs. busybox.  For the tightest
targets we may eventually want a native "multicall" wrapper (one
binary, multiple `main`s dispatched by `argv[0]`), but that is
deferred until we have measurements showing busybox outcompetes the
native set on size.

## Open Questions

- **Multicall or not.**  Keep applets as individual ELFs (simpler,
  easier to `exec`, no argv[0] dispatch), or introduce a PPAP native
  multicall binary once Tier 3 lands?  Decide after Tier 2 with real
  size numbers.
- **When to drop busybox entirely.**  Once Tiers 1–3 land, is the
  remaining busybox surface (`grep`/`sort`/`sed`/`hush`) small enough
  that it still earns its place?  Revisit at that milestone.
- **PicoCalc-only utilities.**  Some future apps (frame buffer demos,
  keyboard test) will be target-specific.  Those live under
  `src/target/<target>/user/` rather than `src/user/` and are out of
  scope here.

## Verification

Each tier's rollout commits include `./scripts/run.sh --test qemu_arm`
and `qemu_m68k` output (pass counts).  Size regression is measured
via `./scripts/build.sh pcxt` romfs output, compared against the
pre-Step-0 baseline.
