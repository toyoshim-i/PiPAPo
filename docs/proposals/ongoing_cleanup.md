# Ongoing Cleanup Backlog

Living list of cleanups surfaced during the
`arch/*` + `target/*` extern-in-`.c` audit and the FreeCOM-on-pcxt
debug session.  Items are roughly grouped by theme; strike through
(or delete) entries as they land.

Last session snapshot: 2026-04-22.

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

Discovered while debugging FreeCOM's `dir` command on pcxt.  None are
strictly required for `command.com` to reach a prompt now, but each
either silences unimpl klogs or unlocks more DOS apps.

- **AH=44h AL=0Ch Generic Character Device Request** — not wired.
  Any meaningful reply depends on CH/CL category/minor sub-codes
  (0x00 unknown, 0x01 COM, 0x03 CON, 0x05 LPT, …) which have
  wildly different semantics.  Falls through to
  DOS_ERR_INVALID_FUNCTION; FreeCOM handles that gracefully, so
  adding it waits for a specific app that probes it.
- **AH=71h (LFN) — implement an AH=71h subset** — with AH=4Eh/4Fh
  now wired, the next step is an AH=71h dispatcher that routes LFN
  sub-functions (AL=3Bh chdir, AL=39h/3Ah mkdir/rmdir, AL=41h delete,
  AL=43h attrs, AL=47h getcwd, AL=56h rename, AL=4Eh/4Fh find, AL=60h
  truename, AL=6Ch extended open) to the existing handlers.  PPAP's
  VFS stores real long names; the SFN AH=4Eh/4Fh path truncates them
  into the 13-byte DTA slot, so LFN is the right path for showing
  full filenames.  Do NOT take the shortcut of replying AX=0x7100
  CF=1 ("not installed") — that forces apps back to the short-name
  API which hides the real names.

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
