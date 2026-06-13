# Ongoing Cleanup Backlog

Low-priority cleanups and deferred refactoring noted in passing.
Sections 1–2 are fix-as-you-touch drift.  Sections 3–9 are
deliberate refactoring PRs with measurable wins, ordered by ROI.
Strike or delete entries as they land.

---

## 1. Layering violations

Two axes have known violations.  Both stay as in-code TODO
comments because the proper fix costs more than the current
breakage warrants.

### 1.1 Target core reaches into VFS (core ↔ VFS)

Three target-core files include VFS driver headers directly:

- [`src/target/x68k/kernel/core/target_x68k.c:40`](../../src/target/x68k/kernel/core/target_x68k.c#L40)
- [`src/target/qemu_rv32/kernel/core/target_qemu_rv32.c`](../../src/target/qemu_rv32/kernel/core/target_qemu_rv32.c) (TODO in `f5c9521`)
- [`src/target/qemu_arm/kernel/core/target_qemu_arm.c`](../../src/target/qemu_arm/kernel/core/target_qemu_arm.c) (TODO in `f5c9521`)

The core ↔ VFS boundary is load-bearing only on ia16 `pcxt`
where the two ship as separate segments behind the `mod_*`
far-call bridge.  All three offenders are non-ia16, so a direct
call links fine today.

The proper fix is to add `mod_vfs.flatblk_init` / `.blkdev_find`
/ `.ramblk_init` entries (with the five-file `mod_*.inc` sync)
and switch the includes.  **Out of scope right now** — no ia16
caller surfaces these paths and the sync churn isn't justified.
If an ia16 port needs these later, that triggers the migration.

### 1.2 ARM driver reaches into a specific target (arch ↔ target)

Three ARM driver files include a pico1calc-specific header:

- [`src/arch/arm_m/kernel/vfs/driver/spi_rpico.c`](../../src/arch/arm_m/kernel/vfs/driver/spi_rpico.c)
- [`src/arch/arm_m/kernel/vfs/driver/i2c_rpico.c`](../../src/arch/arm_m/kernel/vfs/driver/i2c_rpico.c)
- [`src/arch/arm_m/kernel/vfs/driver/spi_lcd_rpico.c`](../../src/arch/arm_m/kernel/vfs/driver/spi_lcd_rpico.c)

`#include "target/pico1calc/kernel/core/pico1calc.h"` — an
arch-level driver reaching into one specific target's internals
(wrong direction).  ARM-only path so the load-bearing ia16
boundary is unaffected.  Bites when a second pico1-class target
with different pin assignments lands and needs the same drivers.

Proper fix: abstract the arch driver behind an arch-level config
interface, or introduce a generic target-config header the
driver queries.

## 2. Header / include hygiene

Project rule: stdlib `#include`s first, then project headers,
alphabetical within each group, all in a single cluster at the
top of the file (after the file's own header).  Two flavours
seen in the wild:

- `.c` under `src/arch/` or `src/target/` with a mid-file
  `#include` — lift to the top cluster on next edit.
- Project `#include`s before stdlib, or out of order within the
  project group — re-sort on next edit.

No master list is kept; the rule is fix-as-you-touch so
unrelated diffs don't accumulate.

## 3. Subsystem bridge helper duplication

[`human68k_bridge.c`](../../src/kernel/core/subsys/human68k/human68k_bridge.c)
(~2300 lines), [`cpm_bridge.c`](../../src/kernel/core/subsys/cpm/cpm_bridge.c)
(~1600), and [`sos_bridge.c`](../../src/kernel/core/subsys/sos/sos_bridge.c)
(~1200) duplicate identical helper functions: `fd_desc()`,
`page_ref()`, `fd_read()`, `fd_write()`, `fd_poll()`,
`fd_ioctl()`, `putc()`, `print()`, plus file-op wrappers —
literal copy-paste with only the prefix changed.  Counted today:
~10/7/11 static helpers per bridge.

Natural extraction is a shared
`src/kernel/core/subsys/subsys_common.{c,h}` that the three
bridges call directly.

**Hold:** the long-term plan is to move subsystem bridges to
userland (tracked in a separate proposal).  Once that lands, the
duplication either disappears (each subsys becomes its own user
binary with its own libc helpers) or moves to userland too.
Revisit this item only after the userland-migration direction
is settled.

## 4. Macro / definition duplication audit

Five (now six, with vfat) VFS filesystems hand-roll the same few
patterns: name comparison loops, vnode alloc-and-fill on lookup,
page-chunked copy loops, readdir entry filling with
`VFS_NAME_MAX` truncation.  The same shape shows up elsewhere in
the tree (constants, small inline helpers, format strings).

Generalize: a one-pass audit of duplicated `#define` macros and
small inline helpers that should have been single-source, then
consolidate into shared headers (`src/kernel/vfs/vfs_util.h`,
`src/common/string_util.h`, etc. as appropriate).

Audit first — known starting list:

- VFS name comparison loops (`while (*a && *a == *b)`) — five
  filesystems each roll their own.
- Vnode alloc-and-fill on lookup — ~6 lines repeated at 5+ call
  sites.
- Page-chunked copy loops in 4 filesystems.
- Readdir entry filling with `VFS_NAME_MAX` truncation in 4
  filesystems.

There will be more.  Audit drives the actual extraction.

## 5. Module system PATCH_CORE auto-generation

`mod_*.inc` is now SSOT for module function **names + indices**,
and both C and asm stubs are generated from it (see
[mod_vfs.h:527-537](../../src/kernel/common/mod/mod_vfs.h#L527-L537)).

The runtime-hazard sync remains: [target_pcxt.c:94+](../../src/target/pcxt/kernel/core/target_pcxt.c#L94)
hand-codes `PATCH_CORE(idx, sym)` for every entry, and a stale
index silently routes VFS→core calls to the wrong slot at
runtime — invalid-opcode panics far from the cause (bit the
codebase at commit `71e697e`).

Fix: drive `PATCH_CORE` from `mod_core.inc` via a third macro
expansion of the same X-macro, eliminating the hand-coded index
list entirely.  Touches just `target_pcxt.c`; the .inc file
already has everything needed.

Also still hand-listed in `.h`: per-function type signatures
(`MOD_FUNC(vfs, ret, name, args...)`).  Folding those into the
`.inc` would make the .inc a true single declaration site;
smaller payoff than the PATCH_CORE risk.

## 6. Syscall dispatch table

[`src/kernel/core/syscall/syscall.c`](../../src/kernel/core/syscall/syscall.c)
is 484 lines containing a ~96-case `switch(nr)` with hand-written
casts of the trap-frame array per case.  Adding a syscall means
inserting a case with the right cast incantation.

Replace with a function-pointer table indexed by syscall number:

```c
typedef long (*syscall_fn_t)(long, long, long, long, long, long);
static const syscall_fn_t syscall_table[SYS_MAX] = {
  [SYS_EXIT]  = (syscall_fn_t)sys_exit,
  ...
};
```

Special cases (CLONE/FORK aliasing, OPENAT) become small
pre-processing wrappers.  Adding a syscall becomes a single
table entry.

## 7. VFS null-object pattern

VFS dispatch wrappers chain 4–5 NULL checks per call (`vn`,
`vn->mount`, `vn->mount->ops`, `vn->mount->ops->X`, plus a user
pointer arg in some cases).  The magic-number part of this item
landed in `62b7c85` (`-2` → `-ENOSYS` with a head comment
explaining the consolidated check).

The remaining work: define a default `vfs_ops_t` whose stub
functions return `-ENOSYS`, and assign it at mount time when
ops are NULL.  That eliminates the function-pointer NULL checks
from the dispatch path entirely and gives every wrapper a single
line of body.  See the head comment on
[vfs_vnode_read in vfs.c](../../src/kernel/vfs/vfs.c#L631).

## 8. eCPU register access tables

[`ecpu_z80.c`](../../src/kernel/core/cpu/ecpu_z80.c) has ~54
case-statement occurrences across paired `get_reg()` / `set_reg()`
switches that mirror each other; adding a register means editing
both switches identically.  m68k has only ~10 occurrences and
benefits less.

Fix (z80 first): a register descriptor table indexed by the
register enum, with `{offset, size}` per entry.  Generic
`get_reg` / `set_reg` index via `offsetof`; synthetic registers
(AF = A<<8 | F) handled by a small fallback.

## 9. Signal delivery arch hooks (low ROI)

[`signal.c`](../../src/kernel/core/signal/signal.c) (852 lines)
has per-arch `#if defined(__m68k__)` / `__arm__` / `__ia16__` /
`__riscv__` blocks for the delivery sequence.  Original
proposal: extract an `arch_signal_ops_t` vtable with
`save_frame` / `restore_frame` / `call_handler` hooks.

Caveat: the per-arch mechanisms are genuinely different (RTE,
IRET, PendSV, trap-frame rewrite).  The "common" core would be
thin and the per-arch implementations would still hold most of
the delivery logic — the abstraction adds layering overhead
without much consolidation.  Lowest ROI of the refactoring
items; revisit only if a new arch port surfaces friction.

## 10. Thread-safety follow-ups

The common kernel thread-safety plan completed its Phase 1-5
hardening and target-matrix baseline.  These are deferred cleanup
items that were explicitly not required to keep that proposal open.

### 10.1 File descriptor edge cases

`fd.c` now protects descriptor allocation/refcounts with `SPIN_FD`
and open-file state with per-file `kmutex_t`, but a few behavioral
edges remain worth auditing:

- killed-process cleanup when descriptor pins are held by interrupted
  blocking file operations;
- whether `fcntl(F_SETFL)` should update nonblocking mode while another
  operation on the same shared file is asleep;
- whether any driver needs to split long blocking I/O from file-state
  mutation so concurrent operations on one open-file instance can make
  progress.

### 10.2 Sleep and wakeup documentation

Blocking paths use shared sleep helpers, but the surrounding contract
could be made easier to audit:

- document which wakeups are level-triggered by rechecking a condition
  and which, if any, are edge-triggered;
- consider a debug assertion for `sched_sleep_current_unlock()` callers
  that pass an invalid spinlock ID;
- consider helper APIs for process teardown paths that currently make
  direct `PROC_BLOCKED` state changes under `SPIN_PROC`.

### 10.3 Filesystem locking refinements

`tmpfs` intentionally uses one coarse sleepable mutex today.  Revisit
only if requirements grow:

- split the lock per mount if multiple tmpfs instances become supported;
- split per-inode locks only if contention becomes meaningful;
- add a concurrent mount/unmount stress test once process-level
  concurrency helpers exist.

### 10.4 x68k IOCS and crash logging

The x68k IOCS path is serialized with `kmutex_t`.  Remaining target-local
hardening ideas:

- add an IRQ-context assertion once there is a common arch helper for it;
- preserve same-owner crash-output bypass behavior;
- consider a hardware-level serial fallback for crash logs that fault
  while IOCS is already held.

### 10.5 Allocator and boot-only registry guards

`kmem_alloc()` / `kmem_free()` remain intentionally unlocked low-level
APIs, with callers responsible for subsystem locking.

- prefer subsystem wrapper APIs if additional pools appear;
- optionally add debug assertions for known pools if lock tracking
  becomes available;
- add a guard or lock if any boot-only registry gains a runtime
  registration user.
