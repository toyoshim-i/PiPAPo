# QEMU Power-Off for Testing

## Motivation

QEMU-based test runs (`qemu_arm`, `qemu_m68k`, `qemu_rv32`, `pcxt`) rely on
an external `timeout` wrapper in `run.sh` to kill the QEMU process after a
fixed deadline (currently 90–180 s depending on target).  This is fragile:

- **Tests that pass quickly still wait for timeout on hang.**  A single stuck
  test blocks the pipeline for the full timeout before being killed.
- **No distinction between clean completion and hang.**  The script parses
  serial output for "ALL TESTS PASSED" after QEMU exits, but the exit code
  from `timeout --kill` is always non-zero on forced termination.
- **Kernel panics and unexpected traps spin forever.**  The guest loops or
  halts but QEMU keeps running until the external timeout fires.
- **Flaky timeout tuning.**  Different targets, test subsets, and host speeds
  require different timeout values, leading to maintenance churn.

QEMU's `isa-debug-exit` device provides a clean solution: the guest writes a
value to a designated I/O port and QEMU exits immediately with a
guest-controlled exit code.

---

## Design

### 1. QEMU device flag

Add `-device isa-debug-exit,iobase=0x501,iosize=2` to the QEMU command line
for all QEMU targets in `run.sh`.  The I/O port address (`0x501`) is chosen to
avoid conflicts with standard PC I/O space.

QEMU exits with code `(value << 1) | 1`.  Writing `0x00` exits with code
`0x01` (success-ish); writing `0x01` exits with code `0x03` (panic).

| Guest write | QEMU exit code | Meaning        |
|-------------|----------------|----------------|
| `0x00`      | `1`            | Clean shutdown |
| `0x01`      | `3`            | Panic / trap   |

### 2. Kernel-side: `target_qemu_poweroff(status)`

Each QEMU target provides a `target_qemu_poweroff(uint8_t status)` function
that writes `status` to the `isa-debug-exit` I/O port.  This is
target-specific because the I/O mechanism differs per architecture:

| Architecture | Mechanism                                          |
|--------------|----------------------------------------------------|
| ARM          | Memory-mapped I/O write to the port address        |
| m68k         | Memory-mapped I/O write (QEMU virt I/O region)     |
| RISC-V       | Memory-mapped I/O write                            |
| i8086 (pcxt) | `outb` to I/O port                                 |

The function is guarded by a build flag or is simply only present in QEMU
target files (it already links per-target `target_*.c`).

### 3. Hook into panic and fault paths

Call `target_qemu_poweroff(1)` from:

- **Kernel panic** — `panic()` / fatal error paths in `main.c`, `proc.c`
- **Unhandled faults** — `HardFault_Handler` (ARM), `bus_error` (m68k),
  unhandled trap (RISC-V), unhandled exception (i8086)

This ensures QEMU exits immediately on any fatal condition rather than
spinning until the external timeout fires.

A weak default `target_qemu_poweroff()` (no-op) in shared code ensures
non-QEMU targets are unaffected.

### 4. Syscall: `SYS_POWEROFF` (0x0B00)

Add a new syscall in the debug/system group:

```c
/* src/common/syscall_nr.h */
#define SYS_POWEROFF 0x0B00
```

The kernel handler calls `target_qemu_poweroff(0)` for a clean shutdown.
On non-QEMU targets, the weak no-op means the syscall simply returns (or
could be wired to a platform-specific halt/reset in the future).

### 5. User-space: `qemu_poff` command

A minimal user-space binary that invokes `SYS_POWEROFF`:

```c
/* tests/user/qemu_poff.c */
#include "syscall.h"

int main(void) {
    syscall0(SYS_POWEROFF);
    /* If we get here, poweroff is not supported — just exit */
    return 0;
}
```

Built into romfs only for QEMU test builds (`PPAP_TESTS`).  Placed at
`/bin/qemu_poff`.

### 6. Test runner integration

At the end of `runtests.c` (and `runtests_ext.c`), after printing the
results summary, exec `qemu_poff`:

```c
    /* Shut down QEMU cleanly */
    execve("/bin/qemu_poff", NULL, NULL);
    /* fallthrough: if qemu_poff is missing, exit normally */
    return failed;
```

This causes QEMU to exit immediately after tests complete, with exit code
`1` (clean).  No external timeout needed for the success path.

### 7. `run.sh` changes

- Add `isa-debug-exit` device to the QEMU command line for all QEMU targets.
- Adjust exit code checks: QEMU exit code `1` = success (guest wrote `0x00`),
  exit code `3` = panic (guest wrote `0x01`).
- Keep the external `timeout` as a safety net but it should rarely fire.
  Consider reducing the timeout values since clean runs now self-terminate.

---

## Scope

- QEMU targets only: `qemu_arm`, `qemu_m68k`, `qemu_rv32`, `pcxt` (when
  running under QEMU).
- No changes to hardware targets (pico1, pico1calc, pico2, pico2rv, x68k,
  xtensa_cc).
- The `SYS_POWEROFF` syscall is available on all targets but is a no-op on
  non-QEMU builds (weak symbol).

## Files to modify

| File | Change |
|------|--------|
| `src/common/syscall_nr.h` | Add `SYS_POWEROFF 0x0B00` |
| `src/kernel/core/syscall/syscall.c` | Handle `SYS_POWEROFF` |
| `src/target/qemu_arm/kernel/core/target_qemu_arm.c` | `target_qemu_poweroff()` |
| `src/target/qemu_m68k/kernel/core/target_qemu_m68k.c` | `target_qemu_poweroff()` |
| `src/target/qemu_rv32/kernel/core/target_qemu_rv32.c` | `target_qemu_poweroff()` |
| `src/target/pcxt/kernel/core/target_pcxt.c` | `target_qemu_poweroff()` |
| `src/target/target.h` | Declare weak `target_qemu_poweroff()` |
| `src/kernel/core/main.c` | Call poweroff on panic (if applicable) |
| Arch fault handlers | Call poweroff on unhandled faults |
| `tests/user/qemu_poff.c` | New: user-space poweroff command |
| `tests/user/runtests.c` | Exec `qemu_poff` after test summary |
| `tests/user/runtests_ext.c` | Same |
| `scripts/run.sh` | Add `isa-debug-exit` device, adjust exit code handling |
| `cmake/user.cmake` | Build `qemu_poff` for test builds |

## Exit code mapping

```
run.sh interprets QEMU exit codes:
  0 → impossible (isa-debug-exit always sets bit 0)
  1 → clean shutdown (guest wrote 0x00) → tests passed (check serial output)
  3 → panic (guest wrote 0x01) → tests failed / kernel panic
124 → timeout fired (external safety net) → hang detected
```

## Development Plan

### Step 1: Kernel infrastructure — syscall and weak default

Add the `SYS_POWEROFF` syscall number and the kernel-side plumbing.

1. `src/common/syscall_nr.h` — add `#define SYS_POWEROFF 0x0B00`
2. `src/target/target.h` — declare `void target_qemu_poweroff(uint8_t status)`
3. `src/target/target_default.c` — add weak no-op:
   `__attribute__((weak)) void target_qemu_poweroff(uint8_t s) { (void)s; }`
4. `src/kernel/core/syscall/syscall.c` — add `case SYS_POWEROFF:` that calls
   `target_qemu_poweroff(0); return;`

**Verify:** builds for all targets (the weak default satisfies the linker for
hardware targets).

### Step 2: Per-target `target_qemu_poweroff()` implementations

Each QEMU target overrides the weak default with an architecture-appropriate
exit mechanism.  The `isa-debug-exit` device (I/O port write) is available on
x86 QEMU; other architectures use their native QEMU exit path.

| Target | Mechanism | Details |
|--------|-----------|---------|
| `pcxt` | `isa-debug-exit` | `outb(status, 0x501)` — I/O port access |
| `qemu_arm` | ARM semihosting `SYS_EXIT` | `bkpt 0xAB` with r0=0x18, r1→exit block; already partially supported via `semihost.c` |
| `qemu_rv32` | `sifive_test` MMIO | Write `0x5555` (pass) or `0x3333` (fail) to `0x100000`; device is built into QEMU virt rv32 |
| `qemu_m68k` | `virt-ctrl` MMIO | Write reset/poweroff command to the virt-ctrl register (verify address from QEMU m68k virt memory map) |

Files:
- `src/target/pcxt/kernel/core/target_pcxt.c`
- `src/target/qemu_arm/kernel/core/target_qemu_arm.c`
- `src/target/qemu_rv32/kernel/core/target_qemu_rv32.c`
- `src/target/qemu_m68k/kernel/core/target_qemu_m68k.c`

**Verify:** build each QEMU target.

### Step 3: QEMU command-line changes in `run.sh`

Add the required QEMU device flags per target:

- `pcxt`: add `-device isa-debug-exit,iobase=0x501,iosize=2`
- `qemu_arm`: add `-semihosting` if not already present (test mode only)
- `qemu_rv32`: no extra flags needed (`sifive_test` is built-in)
- `qemu_m68k`: no extra flags needed (`virt-ctrl` is built-in)

Adjust exit code interpretation in the test section:
- `pcxt`: QEMU exit code `1` = clean, `3` = panic
- Other targets: exit code `0` = clean (semihosting/sifive_test exit with 0)

**Verify:** `run.sh` still starts QEMU correctly for all targets (non-test
mode unaffected).

### Step 4: Hook panic and fault paths

Insert `target_qemu_poweroff(1)` calls at fatal halt points so QEMU exits
immediately instead of spinning.

#### 4a. Kernel panic (`main.c`)

Three `for (;;) arch_wfi()` loops after `PANIC:` messages in `kmain()`:
- `mem_region_init` failure (line ~44)
- Thread 0 stack allocation failure (line ~108)
- No init binary (line ~135)

#### 4b. ARM fault handler (`arm_m_common.c`)

- `kernel_hardfault_dump()` — before the final print, or in boot.S after
  the `b Default_Handler` at the end of the kernel HardFault path
- `arm_crash_handler()` — in the `!p || p->pid == 0` kernel-fault branch,
  before the `while(1)` spin

#### 4c. m68k fault handler (`m68k_common.c`)

- `m68k_crash_handler()` — in the `!p || p->pid == 0` kernel-fault branch
  (currently returns 0, caller halts in boot.S)

#### 4d. RISC-V fault handler (`riscv_common.c`)

- `riscv_exception_handler()` — before the final `for (;;) nop` spin
  (two sites: re-entry guard and end-of-handler)

#### 4e. i8086 fault handler (`i16_common.c`)

- `i16_syscall_dispatch()` — the `for (;;) cli; hlt` after return-IP
  corruption panic

**Verify:** build all QEMU targets; kernel panics should still print their
diagnostic before exiting.

### Step 5: `qemu_poff` user-space command

1. Create `tests/user/qemu_poff.c` — minimal binary that calls
   `syscall0(SYS_POWEROFF)` and falls through to `_exit(0)`.
2. `cmake/user.cmake` — add `qemu_poff` to the test-binary list
   (guarded by `PPAP_TESTS`).
3. Confirm it appears in romfs as `/bin/qemu_poff`.

**Verify:** build a QEMU test target, confirm `/bin/qemu_poff` exists in
the romfs listing.

### Step 6: Test runner integration

Modify `tests/user/runtests.c` and `tests/user/runtests_ext.c`:

After printing the final summary, exec `/bin/qemu_poff`:

```c
    /* Power off QEMU after tests complete */
    execve("/bin/qemu_poff", (void *)0, (void *)0);
    /* fallthrough if qemu_poff not present */
    return failed;
```

Both the `__ia16__` and non-`__ia16__` code paths in `runtests.c` need this
addition.

**Verify:** `./scripts/run.sh --test qemu_arm` — QEMU should exit on its own
after printing "ALL TESTS PASSED", well before the external timeout fires.

### Step 7: `run.sh` exit code handling

Update the test-result logic in `run.sh`:

- Currently: timeout kills QEMU, then grep for "ALL TESTS PASSED"
- After: QEMU self-terminates; interpret exit code:
  - `0` or `1` (depending on target mechanism) = clean exit
  - `3` = panic exit
  - `124` = timeout (safety net, indicates a hang)
- Still grep for "ALL TESTS PASSED" / "SOME TESTS FAILED" in the serial
  output for the final pass/fail verdict — the exit code is supplementary.
- Optionally reduce timeout values since self-termination is the expected path.

**Verify:** full test run on all three QEMU targets:
```sh
./scripts/run.sh --test qemu_arm
./scripts/run.sh --test qemu_m68k
./scripts/run.sh --test qemu_rv32
./scripts/run.sh --test pcxt
```

All should self-terminate promptly after tests complete.

---

## Future extensions

- `SYS_REBOOT` could reuse the same infrastructure for a proper `reboot(2)`
  implementation.
- Hardware targets could wire `SYS_POWEROFF` to chip-specific deep-sleep or
  watchdog reset.
