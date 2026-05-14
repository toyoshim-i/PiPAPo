# `signal.c` Cleanup — Remove Arch `#ifdef` Ladders

**Status:** proposed.  Per the project rule "No `#ifdef` for arch/target
conditionals in shared code" ([`coding_rules.md`](../getting_started/coding_rules.md)),
[`src/kernel/core/signal/signal.c`](../../src/kernel/core/signal/signal.c) currently
violates the convention with two large per-arch `#if defined(__arch__)` ladders
covering ~580 of its 856 lines.

## Goal

Eliminate both `#ifdef` ladders by moving each architecture's
`signal_check` and `sys_rt_sigreturn` bodies into
`src/arch/<arch>/kernel/core/signal/signal_check.c`, leaving the shared file with the
genuinely architecture-independent code only.  No behavior change.

## Current state

| Entry point          | Signature               | Called from                                                                                                            |
|----------------------|-------------------------|------------------------------------------------------------------------------------------------------------------------|
| `signal_check` (ARM) | `void(void)`            | [`src/arch/arm_m/kernel/core/trap.S:152`](../../src/arch/arm_m/kernel/core/trap.S#L152) (`bl signal_check`)             |
| `signal_check` (m68k)| `void(uint32_t *)`      | [`target_qemu_m68k.c:85`](../../src/target/qemu_m68k/kernel/core/target_qemu_m68k.c#L85), [`target_x68k.c:86`](../../src/target/x68k/kernel/core/target_x68k.c#L86) |
| `signal_check` (i16) | `u16(u16, u16)`         | [`src/arch/i16/kernel/core/trap.S:245`](../../src/arch/i16/kernel/core/trap.S#L245)                                    |
| `signal_check` (rv32)| `void(uint32_t *)`      | [`src/arch/riscv/kernel/core/trap.S:229`](../../src/arch/riscv/kernel/core/trap.S#L229)                                |
| `signal_check` (xtensa, stub) | wrong API     | not called — defines `signal_setup_frame(int)` instead                                                                 |
| `sys_rt_sigreturn`   | `long(void)`            | syscall table — uniform C signature, body differs per arch                                                              |

The per-arch `signal_check` signature is **fundamentally different** because each
arch's trap glue passes a different thing.  Unification would require touching
trap.S in three arches — not worth the churn.  Per-arch implementation files
are the natural fix.

## Target layout

### New files

```
src/kernel/core/signal/signal_helper.h     # internal contract: shared → arch
src/arch/arm_m/kernel/core/signal.c        # signal_check, sys_rt_sigreturn
src/arch/m68k/kernel/core/signal.c
src/arch/i16/kernel/core/signal.c
src/arch/riscv/kernel/core/signal.c
src/arch/xtensa/kernel/core/signal.c       # stub returning -ENOSYS until CC-3
```

### Header changes

- **[`signal.h`](../../src/kernel/core/signal/signal.h)** — drop the
  `#if defined(__arch__) … #elif …` prototype ladder
  ([`signal.h:33-39`](../../src/kernel/core/signal/signal.h#L33-L39)) entirely.
  Each arch's `signal_check` prototype moves into
  `src/arch/<arch>/kernel/core/arch.h`.  Public shared API surface in
  `signal.h` is just `sighandler_t`, `SIG_DFL`/`SIG_IGN`, and
  `signal_check_kernel`.

- **`signal_helper.h`** (new) — internal contract included only by
  `src/kernel/core/signal/signal.c` and `src/arch/*/kernel/core/signal.c`.
  Exposes:
  - `static inline int ctz32(uint32_t x);` — moved from `signal.c`, hand-rolled
    (do not use `__builtin_ctz`; see "ctz32 vs `__builtin_ctz`" below).
  - `int signal_default_action(int sig, uint32_t *regs);` — promoted from
    file-local.
  - `int signal_pop_pending(int *sig_out);` — new helper that absorbs the
    duplicated preamble each arch currently repeats (~12 lines × 4 arches).
    Returns:
    - `0` — nothing deliverable, caller returns.
    - `1` — signal popped into `*sig_out`, caller delivers via its arch-specific
      frame setup.
    - the helper internally handles `SIG_IGN` (returns 0, signal consumed) and
      `SIG_DFL` (calls `signal_default_action`; if it returns 1 = "handled",
      return 0; otherwise return 1 with `*sig_out`).
  - `struct kernel_sigaction` if any arch code needs it (currently it doesn't).

### Shared `signal.c` after split

~250 lines, no `#ifdef`:

- `struct kernel_sigaction`
- `signal_default_action`
- `signal_check_kernel`
- `sys_kill`
- `sys_sigaction` (legacy 3-arg, kernel-internal — only ktest uses it)
- `sys_rt_sigaction`
- `sys_rt_sigprocmask`

`ctz32` moves to `signal_helper.h` as `static inline`.

## Step-by-step plan

Each step builds and tests independently; one commit per step.

### Step 1 — Introduce `signal_helper.h`, extern the helpers

- Create [`src/kernel/core/signal/signal_helper.h`](../../src/kernel/core/signal/signal_helper.h)
  with the prototypes above.  No `signal_pop_pending` body yet — that lands in
  step 2.
- Move `ctz32` into the new header as `static inline` (verbatim body).
- Remove `static` from `signal_default_action`; declare it in the header.
- Include the new header in `signal.c`.
- **Verify:** `./scripts/build.sh qemu_arm qemu_m68k qemu_rv32 xtensa_cc pcxt`
  — pure symbol-visibility change, no behavior change.

### Step 2 — Add `signal_pop_pending`

- Implement `signal_pop_pending` in `signal.c` (it stays in the shared file
  because it touches no arch state) and declare in `signal_helper.h`.
- **No call-site changes yet.**  Each arch keeps its inline preamble; the
  helper is unused.  This separates "add the helper" from "switch callers".
- **Verify:** same build matrix.

### Step 3 — Split out ARM

- Create [`src/arch/arm_m/kernel/core/signal.c`](../../src/arch/arm_m/kernel/core/signal.c)
  containing the `#elif defined(__ARM_ARCH) …` block
  ([`signal.c:338-437`](../../src/kernel/core/signal/signal.c#L338-L437) and
  [`signal.c:686-726`](../../src/kernel/core/signal/signal.c#L686-L726)) verbatim,
  including the per-arch design comment.
- Re-route the preamble through `signal_pop_pending`.
- Replace `__builtin_ctz` with `ctz32` for consistency (see "ctz32 vs
  `__builtin_ctz`" below — `__builtin_ctz` lowers to libgcc `__ctzsi2` on
  Cortex-M0+ and we avoid libgcc per
  [`feedback_no_libgcc.md`](file:///home/toyoshim/.claude/projects/-home-toyoshim-Work-PPAP/memory/feedback_no_libgcc.md)).
- Move the `signal_check` prototype to
  [`src/arch/arm_m/kernel/core/arch.h`](../../src/arch/arm_m/kernel/core/arch.h).
  trap.S already has its own `extern signal_check` knowledge via the linker;
  no asm change.
- `extern uint32_t arm_exc_return[]; int core_id(void);` — verify they come
  from a header already (no inline `extern` in the new `.c` file per
  [`feedback_no_in_function_externs.md`](file:///home/toyoshim/.claude/projects/-home-toyoshim-Work-PPAP/memory/feedback_no_in_function_externs.md)).
- Remove the `__ARM_ARCH` block from shared `signal.c`.
- Wire the new file into the ARM block of
  [`cmake/kernel.cmake:21-25`](../../cmake/kernel.cmake#L21-L25).
- **Verify:** `./scripts/build.sh qemu_arm && ./scripts/run.sh --test qemu_arm`
  (expect 24/24), plus `./scripts/build.sh pico1calc pico2`.

### Step 4 — Split out m68k

- Create [`src/arch/m68k/kernel/core/signal.c`](../../src/arch/m68k/kernel/core/signal.c)
  with the `__m68k__` blocks
  ([`signal.c:119-210`](../../src/kernel/core/signal/signal.c#L119-L210) and
  [`signal.c:612-655`](../../src/kernel/core/signal/signal.c#L612-L655)).
- Move `extern volatile uint32_t m68k_trap_frame_sp;` and the `M68K_TRAP_FRAME_*`
  defines into [`src/arch/m68k/kernel/core/arch.h`](../../src/arch/m68k/kernel/core/arch.h)
  (or a sibling `trap_frame.h`); drop the inline `extern`.
- Move the `signal_check` prototype to the arch header.  Update
  [`target_qemu_m68k.c`](../../src/target/qemu_m68k/kernel/core/target_qemu_m68k.c)
  and [`target_x68k.c`](../../src/target/x68k/kernel/core/target_x68k.c) includes
  if they were getting it via `kernel/core/signal/signal.h` (they were —
  both should now include the arch header for `signal_check`'s prototype).
- Re-route the preamble through `signal_pop_pending`.
- Remove the `__m68k__` blocks from shared `signal.c`.
- Wire into [`cmake/kernel.cmake:29-36`](../../cmake/kernel.cmake#L29-L36).
- **Verify:** `./scripts/build.sh qemu_m68k && ./scripts/run.sh --test qemu_m68k`
  (expect 19/19), plus an XEiJ boot check for x68k.

### Step 5 — Split out i16 (pcxt)

- Create [`src/arch/i16/kernel/core/signal.c`](../../src/arch/i16/kernel/core/signal.c)
  with the `__ia16__` blocks
  ([`signal.c:212-336`](../../src/kernel/core/signal/signal.c#L212-L336) and
  [`signal.c:657-684`](../../src/kernel/core/signal/signal.c#L657-L684)).
- Move `extern volatile uint16_t i16_trap_frame_sp;` and `I16_*_BYTES` /
  `I16_USER_SEG_PAGES` into the arch header.
- Move the `signal_check` prototype to the arch header.
- Re-route the preamble through `signal_pop_pending`.
- Remove the `__ia16__` blocks from shared `signal.c`.
- Wire into [`src/target/pcxt/CMakeLists.txt`](../../src/target/pcxt/CMakeLists.txt)
  (pcxt has its own list; place the new file in the same kernel segment as
  the rest of `signal.c` — the **core** segment per the module split).
- **Heed `project_pcxt_size.md`** — measure segment sizes before/after.
  Identical instructions but file-order changes can shift symbol layout.
- **Verify:** `./scripts/build.sh pcxt` and `./scripts/run.sh --test pcxt`
  (or whatever the current pcxt test invocation is).

### Step 6 — Split out RISC-V

- Create [`src/arch/riscv/kernel/core/signal.c`](../../src/arch/riscv/kernel/core/signal.c)
  with the `__riscv` blocks
  ([`signal.c:439-534`](../../src/kernel/core/signal/signal.c#L439-L534) and
  [`signal.c:728-756`](../../src/kernel/core/signal/signal.c#L728-L756)).
- Move `extern volatile uint32_t rv32_trap_frame_sp;` and `RV32_TF_*` into the
  arch header.
- Move the `signal_check` prototype to the arch header.
- Re-route the preamble through `signal_pop_pending`.
- Remove the `__riscv` blocks from shared `signal.c`.
- Wire into [`cmake/kernel.cmake:40-44`](../../cmake/kernel.cmake#L40-L44).
- **Verify:** `./scripts/build.sh qemu_rv32 && ./scripts/run.sh --test qemu_rv32`
  (expect the current 17 passing — no regression), plus `pico2rv` build.

### Step 7 — Split out Xtensa (stub)

- Create [`src/arch/xtensa/kernel/core/signal.c`](../../src/arch/xtensa/kernel/core/signal.c)
  with a correctly-named `void signal_check(void)` stub (ARM-shape) and
  `long sys_rt_sigreturn(void) { return -(long)ENOSYS; }`.  The current
  file defines `signal_setup_frame(int)` — fix the name so the API matches
  the prototype declared in the xtensa `arch.h`.
- Add the `signal_check` prototype to
  [`src/arch/xtensa/kernel/core/arch.h`](../../src/arch/xtensa/kernel/core/arch.h).
- Remove the `__xtensa__` blocks from shared `signal.c`.
- Wire into [`cmake/kernel.cmake:47-53`](../../cmake/kernel.cmake#L47-L53).
- **Verify:** `./scripts/build.sh xtensa_cc` builds and links.

### Step 8 — Final shared-file cleanup (revised)

The original plan put per-arch `signal_check` prototypes into each arch's
`arch.h`.  Reviewer pushback: `arch.h` is the forward-declaration header
for the arch-abstraction API (`arch_yield`, `arch_wfi`, etc.); cramming
`signal_check` there breaks the header-pairs-with-its-`.c` convention.
Reviewer also pointed out that `signal_helper.h` was the wrong shape —
two of its three contents pair with the shared `signal.c` (so they
belong in `signal.h`) and `ctz32` is a bit-twiddling helper unrelated
to signals (so it belongs in a generic bitops header).  Revised step:

**Per-arch `signal_check.{c,h}`:** rename each per-arch `signal.c` →
`signal_check.c` and pair it with a new `signal_check.h` in the same
directory.  Two reasons:
  - Header name = the single function it declares (`signal_check`),
    so the file's purpose is unambiguous from the filename alone.
  - Avoids the basename collision that would arise from putting a
    per-arch `signal.h` next to the shared
    `src/kernel/core/signal/signal.h` (different paths but identical
    leafname — confusing).

Non-arch callers reach the per-arch header via
`#include "kernel/core/signal/signal_check.h"`, which resolves to the
appropriate per-arch overlay (-I `src/arch/<arch>` ahead of -I `src`),
the same dispatch mechanism `kernel/core/arch.h` uses today.  The
per-arch files live under `src/arch/<arch>/kernel/core/signal/` —
mirroring the shared `src/kernel/core/signal/` layout.  The two
C callers of `signal_check`
([`target_qemu_m68k.c`](../../src/target/qemu_m68k/kernel/core/target_qemu_m68k.c)
and [`target_x68k.c`](../../src/target/x68k/kernel/core/target_x68k.c))
use this path; each per-arch `signal_check.c` includes its own paired
header via the same path (own-header-first rule satisfied because the
overlay resolves to the file in the same directory).

**`bitops.h`:** create `src/kernel/common/bitops.h` containing only
`static inline int ctz32(uint32_t x)`.  Generic bit-twiddling helpers;
future `clz32` / `popcount32` collocate cleanly.  Hand-rolled — no
`__builtin_ctz` / libgcc `__ctzsi2` dependency.

**Promote helper prototypes into `signal.h`:** `signal_default_action`
and `signal_pop_pending` are implemented in the shared `signal.c`, so
their declarations belong in the paired `signal.h`, not in a separate
"helper" header.

**Delete `signal_helper.h`** — its three contents are redistributed
above.

**Drop the ladder** from
[`signal.h`](../../src/kernel/core/signal/signal.h): per-arch
`signal_check` prototypes now live in per-arch headers.

**Update file-header docs** in `signal.c` and `signal.h` to reflect
the post-cleanup state.

**Per-arch `signal_check.c` include block** after this step:
```c
#include "kernel/core/signal/signal_check.h"  /* own header (overlay-resolved) */

#include <stdint.h>

#include "common/errno.h"
#include "kernel/core/arch.h"           /* <arch>_trap_frame_sp etc. */
#include "kernel/core/proc/proc.h"
#include "kernel/core/signal/signal.h"  /* signal_pop_pending */
#include "kernel/core/syscall/syscall.h"
```
(`kernel/common/bitops.h` is needed only in the shared `signal.c` —
the per-arch files go through `signal_pop_pending` and don't call
`ctz32` directly.)

**Verify:** full QEMU test matrix —
`qemu_arm` (24/24), `qemu_m68k` (24/24), `qemu_rv32` (17/17), plus
builds for `pico1calc`, `pico2`, `pico2rv`, `xtensa_cc`, `pcxt` kernel.

## `ctz32` vs `__builtin_ctz`

`__builtin_ctz` lowers to a libgcc `__ctzsi2` runtime call on every PPAP target
except ARM v7-M+ (Cortex-M3, M33) and RV32 with Zbb — neither of which is the
common case across the supported board set.  Per
[`feedback_no_libgcc.md`](file:///home/toyoshim/.claude/projects/-home-toyoshim-Work-PPAP/memory/feedback_no_libgcc.md),
PPAP avoids libgcc references because distro libgcc is built with flags PPAP
cannot rely on (no-PIC, wrong CPU baseline — e.g. Debian's `m68k-linux-gnu`
libgcc is 68020+ and faults on the m68000 used by qemu virt and X68000, wrong
multilib, etc.).

Decision: keep the hand-rolled `ctz32` as `static inline` in `signal_helper.h`,
switch the ARM branch from `__builtin_ctz` to `ctz32` for consistency.  On
v7-M+ the compiler still recognizes the pattern and emits the same
`CLZ`-based codegen; on v6-M / m68k / RV32-no-Zbb / i16 we get ~30 bytes of
inline code instead of a `__ctzsi2` call.

## What is *not* in scope

- No behavior change.  Pure file/symbol relocation + extern cleanup.
- No unification of `signal_check`'s signature across arches.
- No fix for the Xtensa CC-3 implementation gap.  The stub stays a stub,
  just correctly named.
- No changes to user-space `sa_restorer` trampolines.

## Risks

- **pcxt size budget** ([`project_pcxt_size.md`](file:///home/toyoshim/.claude/projects/-home-toyoshim-Work-PPAP/memory/project_pcxt_size.md))
  — even identical i16 instructions can shift symbol layout across segments
  with file-order changes.  Measure before/after step 5.
- **Inline `extern`s** in current `signal.c` ([`signal.c:158`](../../src/kernel/core/signal/signal.c#L158),
  [`signal.c:250`](../../src/kernel/core/signal/signal.c#L250),
  [`signal.c:485`](../../src/kernel/core/signal/signal.c#L485)) — moving each
  to an arch header may surface a redeclaration clash with the matching `.S`
  file's own external symbol.  Per
  [`feedback_no_in_function_externs.md`](file:///home/toyoshim/.claude/projects/-home-toyoshim-Work-PPAP/memory/feedback_no_in_function_externs.md)
  the right place is the header, even if it requires a one-line `.S` adjustment.
- **ktest comment** at [`tests/kernel/ktest.c:1783`](../../tests/kernel/ktest.c#L1783)
  ("`signal_setup_frame` is static") remains accurate — the symbol stays
  `static` inside `src/arch/arm_m/kernel/core/signal.c`.  No test change.

## Commit scope

One commit per step, scoped to match
[`feedback_commit_scope.md`](file:///home/toyoshim/.claude/projects/-home-toyoshim-Work-PPAP/memory/feedback_commit_scope.md):

```
signal: introduce signal_helper.h
signal: add signal_pop_pending dedup helper
signal/arm_m: extract arch-specific delivery to src/arch/arm_m
signal/m68k: extract arch-specific delivery to src/arch/m68k
signal/i16: extract arch-specific delivery to src/arch/i16
signal/riscv: extract arch-specific delivery to src/arch/riscv
signal/xtensa: extract stub to src/arch/xtensa, fix API name
signal: remove arch-overview comment, finalize signal.h
```

## Lifecycle

Per [`reference_proposal_lifecycle.md`](file:///home/toyoshim/.claude/projects/-home-toyoshim-Work-PPAP/memory/reference_proposal_lifecycle.md):
delete this file once step 8 lands.
