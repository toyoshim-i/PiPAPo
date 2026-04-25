# Ongoing Cleanup Backlog

Living list of cleanups surfaced during the
`arch/*` + `target/*` extern-in-`.c` audit and the FreeCOM-on-pcxt
debug session.  Items are roughly grouped by theme; strike through
(or delete) entries as they land.

Last session snapshot: 2026-04-22.  Section 1 (extern cleanup)
fully landed.  Section 2 (MSDOS stubs) reduced to the two items
below.  Section 3 (header / include hygiene) is opportunistic.

---

## 1. Layering violations (core ↔ VFS, arch ↔ target) — low priority

Module-boundary enforcement is load-bearing only for the ia16
pcxt target, where core and VFS ship as separate segments and any
cross-module call must go through the `mod_*` far-call bridge.
On every other arch the split is purely organisational — a
core→VFS direct call links fine.  The items below therefore stay
as documented TODO comments in code; **adding new `mod_vfs` /
`mod_core` entries just to legalise them is explicitly out of
scope**.  If an existing `mod_*` entry acquires a matching
caller later, switch the relevant file over in passing.

### 1.1 `flatblk_init` / `blkdev_find` / `ramblk_init` in target core code

Three target-core files reach into the VFS module by including its
driver headers directly:

- [`src/target/x68k/kernel/core/target_x68k.c:40`](../../src/target/x68k/kernel/core/target_x68k.c#L40)
- [`src/target/qemu_rv32/kernel/core/target_qemu_rv32.c`](../../src/target/qemu_rv32/kernel/core/target_qemu_rv32.c) (TODO added in `f5c9521`)
- [`src/target/qemu_arm/kernel/core/target_qemu_arm.c`](../../src/target/qemu_arm/kernel/core/target_qemu_arm.c) (TODO added in `f5c9521`)

All three are non-ia16 targets, so the violation does not break a
real module boundary today — the TODO comments are just a
reminder should an ia16 port ever need these code paths.

### 1.2 `arch/arm_m` → `target/pico1calc` include

- [`src/arch/arm_m/kernel/vfs/driver/spi_rpico.c`](../../src/arch/arm_m/kernel/vfs/driver/spi_rpico.c)
- [`src/arch/arm_m/kernel/vfs/driver/i2c_rpico.c`](../../src/arch/arm_m/kernel/vfs/driver/i2c_rpico.c)
- [`src/arch/arm_m/kernel/vfs/driver/spi_lcd_rpico.c`](../../src/arch/arm_m/kernel/vfs/driver/spi_lcd_rpico.c)

Each `#include "target/pico1calc/kernel/core/pico1calc.h"` — an
arch-level driver reaching into one specific target's internals.
ARM-only code path, same low-priority classification.  A proper
fix would either abstract the arch driver or introduce a generic
target-config header it queries.

## 2. MS-DOS subsystem stubs

### 2.1 AH=44h AL=0Ch Generic Character Device Request — deferred

Not wired.  AL=0Ch is an IOCTL meta-call that takes `BX=handle`,
`CH=category` (0x00 unknown, 0x01 COM, 0x03 CON, 0x05 LPT, 0x48
network, …) and `CL=minor function`, each (CH,CL) pair its own
protocol with its own parameter block at `DS:DX`.  Implementing it
means picking which (category, minor) pairs to support; each is a
separate stub and no single default reply is meaningful.

FreeCOM falls through `DOS_ERR_INVALID_FUNCTION` gracefully.  Pick
this up when a specific DOS app surfaces a failure that traces to
AL=0Ch.

### 2.2 AH=71h LFN subset — plan

**Why.**  PPAP's VFS stores real long filenames.  The SFN
FindFirst path (AH=4Eh/4Fh) truncates them into the 13-byte DTA
slot, which is lossy.  The DOS 7 / Win95 LFN API (entered via
AH=71h, sub-function in AL) is the channel that surfaces long
names intact.  Do NOT take the shortcut of replying AX=0x7100
CF=1 ("not installed") — that forces LFN-aware apps back onto the
short-name API and hides the real names they would otherwise
fetch.

**Why it isn't "just route to SFN handlers."**  Each LFN
sub-function differs from its SFN counterpart in at least one of
three ways:

- **Register conventions** — AL=60h (TrueName) uses
  `DS:SI = input` + `ES:DI = output`, unlike SFN variants that
  use DS:DX only.  AL=A1h (FindClose) takes `BX = find handle`.
- **Result buffer layout** — AL=4Eh under AH=71h writes a
  ~318-byte `WIN32_FIND_DATA`-style struct at `ES:DI`, not the
  128-byte DTA.  Long-name field is 260 bytes.
- **Handle model** — LFN Find uses a numeric find handle in AX
  with support for concurrent finds; SFN Find is DTA-state-based
  with one-per-process semantics.

**Phased rollout.**  Each phase is a self-contained commit / review
unit.  Phases are ordered by value-for-effort; later phases can be
skipped if no caller appears.

Phase L1 — **Dispatcher + AL=60h TrueName.**  Landed.  Adds the
`case 0x71` in `dos_int21h_dispatch`, a sub-dispatcher on AL, and
TrueName = path normalisation via the existing
`dos_resolve_user_path` + copy-out through `cpu_ops->write8`.
Output is the VFS-form absolute path (not DOS "C:\FOO"); apps that
parse for a drive letter will need to wait for a later phase to
wrap the result.

Phase L2 — **AL=4Eh / 4Fh / A1h LFN Find family.**  Landed.
Reuses the SFN `find_fd` / `find_pattern` state (single concurrent
find per process — no handle table yet; AL=4Eh always returns
handle 1, AL=4Fh / A1h ignore `BX`).  Shares the glob matcher and
dirent reader from the SFN path; adds `dos_find_fill_lfn` that
writes a 318-byte `WIN32_FIND_DATA` frame at `ES:DI` with full
long names and both SI=0 (FILETIME) + SI=1 (DOS date/time)
formats.  Multiple-concurrent-find support (real handle table)
deferred until a caller surfaces it.

Phase L3 — **AL=43h LFN attrs, AL=3Bh/41h/56h LFN
chdir/delete/rename.**  Small wrappers over the existing SFN
handlers once the LFN path-copy and result-copy primitives from L1
are in.

Phase L4 — **AL=6Ch Extended Open/Create.**  Landed.  BX (low 3 bits
= access), DX action code (low nibble: if-exists 0/1/2 fail/open/
truncate; high nibble: if-not-exists 0/1 fail/create), DS:SI = path
(note SI, not DX).  Pre-`mod_vfs.lookup` enforces "must (not) exist"
constraints (PPAP fcntl has no O_EXCL) and computes CX = action
taken (1=opened, 2=created, 3=truncated).  CX-attribute and DI
(alias hint) ignored; sharing/inheritance bits in BX accepted and
discarded.

Phase L5 (optional) — **AL=39h/3Ah/47h LFN mkdir/rmdir/getcwd.**
The remaining SFN-equivalent sub-functions.  Trivial wrappers;
land together if a user surfaces them.

**Out of scope for all phases:** AL=A6h LFN Get File Info By
Handle, AL=A7h Convert File Time to/from DOS Time, AL=A8h Generate
Short Name, AL=A9h Redirector queries.  None are required for
typical DOS-on-PPAP use.

## 3. Header / include hygiene found while working

Items I noticed but deliberately did not fix when outside the
current commit's scope; the project rule is to fix them when next
editing the file.

- Any `.c` under `src/arch/` or `src/target/` that has a mid-file
  `#include` should get lifted to the top cluster when next touched.
- Include order: stdlib-first-then-project, alphabetical within
  group.  Several files had project includes before stdlib; fix on
  next touch.

---

## Completed

- **2026-04-22** — Section 1 extern cleanup (all 52 externs across
  the nine `src/arch/` + `src/target/` `.c` files landed as commits
  `13cc68c`, `f5c9521`, `874e10d`, `1e205df`, `6180967`, `0c91c66`,
  `4ed23d2`, `8ab6ca5`, `0fa29c7`; pre-commit guard added in
  `5aa6260`).  Only remaining extern is `target_pcxt.c`'s
  `vfs_fptrs[]` — allowlisted in the guard because it's an
  asm-defined array with a single C writer.
- **2026-04-22** — MSDOS AH=43h Get/Set File Attributes (`40b6ce7`),
  AH=44h AL=01/06/07/08/09/0B (`9e51f12`), AH=1Ah/2Fh Set/Get DTA
  (`de9b98e`), AH=4Eh/4Fh FindFirst/FindNext (`7e2e618` + stat-fill
  in `61d168f`), and a covering test (`b3e7831`).
