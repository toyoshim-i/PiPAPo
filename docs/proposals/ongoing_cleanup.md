# Ongoing Cleanup Backlog

Living list of cleanups surfaced during the
`arch/*` + `target/*` extern-in-`.c` audit and the FreeCOM-on-pcxt
debug session.  Items are roughly grouped by theme; strike through
(or delete) entries as they land.

Last session snapshot: 2026-04-26.  Section 1 (extern cleanup)
fully landed.  Section 2 (MSDOS stubs) reduced to §2.1 only —
the LFN subset planned in §2.2 landed across phases L1–L5.
Section 3 (header / include hygiene) is opportunistic.

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

### 2.1 AH=44h AL=0Ch Generic Character Device Request — explicit stub

Landed as an explicit `case 0x0C` in `dos_ioctl` returning
`DOS_ERR_INVALID_FUNCTION` plus a `DOS_DBG`-gated klogf naming the
(CH=category, CL=minor) pair so a failing app can be traced.

The same commit introduced `DOS_DBG`: an envp-driven gate
(`DBG_MSDOS=` with a non-empty, non-"0" value enables it) that also
silences the existing AH=25h vector-protected/save-table-full and
INT 21h unimpl-AH messages.  Off by default — DOS apps probe a wide
AH/AL surface and the noise drowns legitimate output.

A real (CH,CL) implementation is still gated on a specific app
surfacing a failure; pick it up then.

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
- **2026-04-26** — MSDOS AH=71h LFN subset across phases L1–L5:
  dispatcher + AL=60h TrueName (`e21ae63` + `fc37e3d`), AL=4Eh/4Fh/
  A1h Find family (`f17989b` + `d677328`), AL=43h/3Bh/41h/56h aliases
  (`0ffacee` + `a55973b`), AL=6Ch Extended Open/Create (`f9cac88` +
  `ff804ba`), AL=39h/3Ah/47h mkdir/rmdir/getcwd (`7d31003` +
  `f9f03f5`).  Plus the latent dos_stat_entry near-pointer bug
  surfaced during L4 debugging (`6e9fc0d` + `93e3280`) and the §2.1
  AH=44h AL=0Ch explicit stub with DBG_MSDOS-gated unimpl logging
  (`f0cad5e`).
