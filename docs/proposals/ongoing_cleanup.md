# Ongoing Cleanup Backlog

Living list of cleanups surfaced during the
`arch/*` + `target/*` extern-in-`.c` audit and the FreeCOM-on-pcxt
debug session.  Items are roughly grouped by theme; strike through
(or delete) entries as they land.

Last session snapshot: 2026-04-22.

---

## 1. Layering violations (core ↔ VFS, arch ↔ target)

Flagged during the extern audit but parked because the fix is
larger than moving a declaration to a header.

### 1.1 `flatblk_init` / `blkdev_find` / `ramblk_init` in target core code

Three target-core files reach into the VFS module by including its
driver headers directly:

- [`src/target/x68k/kernel/core/target_x68k.c:40`](../../src/target/x68k/kernel/core/target_x68k.c#L40)
- [`src/target/qemu_rv32/kernel/core/target_qemu_rv32.c`](../../src/target/qemu_rv32/kernel/core/target_qemu_rv32.c) (TODO added in `f5c9521`)
- [`src/target/qemu_arm/kernel/core/target_qemu_arm.c`](../../src/target/qemu_arm/kernel/core/target_qemu_arm.c) (TODO added in `f5c9521`)

Proper fix: add the needed functions (`flatblk_init`, `blkdev_find`,
`ramblk_init`, `BLKDEV_SECTOR_SIZE`) as `MOD_FUNC` entries in
`kernel/common/mod/mod_vfs.h`, update the five-file `mod_vfs.inc`
sync (parallel of the existing `mod_core.inc` five-file sync), and
update each target-core caller to go through `mod_vfs.*`.

### 1.2 `arch/arm_m` → `target/pico1calc` include

- [`src/arch/arm_m/kernel/vfs/driver/spi_rpico.c`](../../src/arch/arm_m/kernel/vfs/driver/spi_rpico.c)
- [`src/arch/arm_m/kernel/vfs/driver/i2c_rpico.c`](../../src/arch/arm_m/kernel/vfs/driver/i2c_rpico.c)
- [`src/arch/arm_m/kernel/vfs/driver/spi_lcd_rpico.c`](../../src/arch/arm_m/kernel/vfs/driver/spi_lcd_rpico.c)

Each `#include "target/pico1calc/kernel/core/pico1calc.h"` — an
arch-level driver reaching into one specific target's internals.
Same layering-violation class.  Needs either an abstraction in the
arch driver or a generic target-config header the arch driver
queries.

## 2. MS-DOS subsystem stubs

Discovered while debugging FreeCOM's `dir` command on pcxt.  None are
strictly required for `command.com` to reach a prompt now, but each
either silences unimpl klogs or unlocks more DOS apps.

- **AH=43h Get/Set File Attributes** — `AL=00h` (get) + `AL=01h` (set).
  Minimum-viable stub can stat the resolved path and return
  `0x20` (archive) / `0x10` (directory).  Documented in
  `docs/proposals/msdos_subsystem.md` Phase 1 TODO.
- **AH=44h AL≠0** — today only `AL=00h` (Get Device Info) is wired.
  Adding `AL=01h`, `06h`, `07h`, `08h`, `09h`, `0Bh`, `0Ch` covers
  the rest of the subset FreeCOM probes at startup.
- **AH=4Eh / 4Fh FindFirst / FindNext** — blocks `dir` listing output
  from appearing; FreeCOM currently runs `dir` silently because the
  builtin bails out when FindFirst returns "invalid function".
  Needs a DTA layout, 8.3 pattern matching, and an open-dir-handle
  per process.  Phase D-7 in `msdos_subsystem.md`.
- **AH=71h (LFN) — proper "not installed" reply** — current fall-through
  returns `AX=1 CF=1`.  DOS spec for LFN-not-installed is `AX=0x7100
  CF=1`; some apps only recognize that form.  Can be handled in the
  dispatcher's default branch as a special case.

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
