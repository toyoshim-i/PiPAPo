# Testing

PPAP has three categories of tests, each with its own framework,
build process, and execution environment.

| Category | Location | Framework | Runs on | Build flag |
|----------|----------|-----------|---------|------------|
| Host unit tests | `tests/host/` | `test_framework.h` | Developer machine | Always |
| Kernel integration tests | `tests/kernel/` | `ktest.h` | QEMU / hardware | `--test` |
| User-space tests | `tests/user/` | `utest.h` | QEMU / hardware | `--test` |

## Quick start

```bash
# Host unit tests (no cross-compiler needed)
./scripts/test.sh

# On-target tests (ARM, requires qemu-system-arm)
./scripts/run.sh --test

# On-target tests (m68k, requires qemu-system-m68k)
./scripts/run.sh --test qemu_m68k

# On-target tests (RISC-V, requires qemu-system-riscv32)
./scripts/run.sh --test qemu_rv32

# On-target tests (PC/XT i16, requires qemu-system-i386)
./scripts/run.sh --test pcxt

# On-target tests (Xtensa, requires M5Stack CardComputer attached)
./scripts/run.sh --test xtensa_cc

# Extended on-target tests (ARM lane with extra user tests)
./scripts/run.sh --test-extended qemu_arm

# Run only tests matching a substring
./scripts/run.sh --test --filter=pipe

# Also run tests marked FLAKY
./scripts/run.sh --test --flaky

# Everything at once (host + build all targets + QEMU tests)
./scripts/test.sh --all

# Everything + extended QEMU lanes
./scripts/test.sh --all --extended
```

## Host unit tests

Pure C unit tests compiled with the system gcc/clang. No cross-compiler
toolchain needed. Good for testing kernel modules that have no hardware
dependencies (memory allocators, path helpers, CPU interpreters).

### Framework: `test_framework.h`

```c
#include "test_framework.h"

static void test_example(void) {
    ASSERT(1 + 1 == 2, "basic math");
    ASSERT_EQ(page_alloc(), expected_addr);
    ASSERT_NOT_NULL(ptr);
}

int main(void) {
    TEST_GROUP("Example tests");
    RUN_TEST(test_example);
    TEST_SUMMARY();
}
```

Macros:
- `ASSERT(cond, msg)` — check condition, print file:line on failure
- `ASSERT_EQ(a, b)` — compare values, print both on mismatch
- `ASSERT_NULL(p)` / `ASSERT_NOT_NULL(p)` — pointer checks
- `RUN_TEST(fn)` — run a test function
- `TEST_GROUP(name)` — section header
- `TEST_SUMMARY()` — print totals, return exit code

### Files

| File | Tests |
|------|-------|
| `test_kmem.c` | `kmem_pool` object allocator |
| `test_h68k_path.c` | Human68k path translation and errno mapping |
| `test_ecpu_z80.c` | Z80 emulator (85 tests: all instruction groups) |
| `test_ecpu_m68k.c` | m68k emulator (instruction groups, addressing modes) |

### Build system

`tests/host/CMakeLists.txt` defines one executable per host-safe module.
Tests that depend on target architecture or kernel-only state now live in
the on-target user test suite instead of being forced through the host build.

```bash
# Manual build
cmake -S tests/host -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

### Adding a new host test

1. Create `tests/host/test_foo.c` with `#include "test_framework.h"`
2. Add to `tests/host/CMakeLists.txt`:
   ```cmake
   add_executable(test_foo ${SRC}/kernel/path/foo.c test_foo.c)
   target_include_directories(test_foo PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
   add_test(NAME foo COMMAND test_foo)
   ```
3. Run `./scripts/test.sh`

## Kernel integration tests

On-target tests that exercise kernel subsystems with real VFS, page
allocator, pipe, signal, and filesystem infrastructure. Compiled into
the kernel image when `PPAP_TESTS=ON` and run before the scheduler starts.

### Framework: `ktest.h` / `ktest.c`

Tests call kernel APIs directly (`sys_open`, `sys_read`, `sys_write`,
`sys_pipe`, etc.) and report results via `uart_puts()`.

```c
static void my_integration_test(void)
{
    uart_puts("\n=== My integration tests ===\n");
    test_pass = 0;
    test_fail = 0;

    long fd = sys_open("/etc/hostname", O_RDONLY, 0);
    test_report("open /etc/hostname", fd >= 0);
    if (fd >= 0) sys_close(fd);

    total_pass += test_pass;
    total_fail += test_fail;
}
```

### Execution flow

1. `./scripts/build.sh --test qemu_arm` sets `PPAP_TESTS=ON`
2. CMake adds `ktest.c` to the kernel build and defines `PPAP_TESTS=1`
3. After VFS mount, `target_post_mount()` calls `ktest_run_all()`
4. All test suites run, printing `TEST: name ... PASS/FAIL` to UART
5. After kernel tests, `target_init_path()` returns:

**Note:** `qemu_rv32` has `ktest_run_all()` disabled — the blkdev/VFAT
kernel tests expect a FAT32 ramblk image that rv32 does not embed, and
the 18 test failures corrupt kernel state, crashing init startup.
`pcxt` does not run kernel tests (no `ktest_run_all()` call in
`target_post_mount()`).
   - `/bin/runtests` in normal test builds
   - `/bin/runtests_ext` in extended test builds
   (instead of `/sbin/init`), launching the user-space test runner

### Adding a kernel test suite

1. Add a `static void foo_integration_test(void)` function in `ktest.c`
2. Call it from `ktest_run_all()`

## User-space tests

On-target tests that run as real user processes. Each test is a
standalone ELF binary compiled with PIC, linked against `crt0.o` and
`syscall.o` (raw SVC stubs — no libc).

### Framework: `utest.h`

```c
#include "utest.h"

int main(void)
{
    void *p = brk((void *)0);
    UT_ASSERT(p != (void *)0, "initial brk non-zero");

    UT_ASSERT_EQ(1 + 1, 2);

    UT_SUMMARY("test_example");
}
```

Macros:
- `UT_ASSERT(cond, msg)` — check condition; print `FAIL` + file:line
- `UT_ASSERT_EQ(a, b)` — compare values; print expected/actual
- `UT_PRINT(s)` — write a string literal to stdout
- `UT_SUMMARY(name)` — print totals, `return 0` or `return 1`

Integer printing uses subtraction (power-of-10 lookup) — no hardware
divide and no libgcc dependency.

### Test runner: `runtests.c`

Sequentially `vfork` + `execve` each test binary, collect exit status.
Every log line is prefixed with `[T+S.CC]` (elapsed seconds and
centiseconds since the runner started, 10 ms resolution). `PASS`/`FAIL`
lines also include per-test elapsed time. Final summary: counts +
`ALL TESTS PASSED` or `SOME TESTS FAILED`.

The test list is initialised at runtime (not as a static array) because
PIC binaries cannot have initialised pointer arrays — the ELF loader
only relocates GOT entries, not arbitrary data pointers.

**Per-test flags**

Each entry in the test list carries a flag:

| Flag | Meaning | Counted as |
|------|---------|------------|
| `TEST_ENABLED` | Run normally | run / passed / failed |
| `TEST_DISABLED` | Skip — broken or fix pending | skipped |
| `TEST_UNSUPPORTED` | Skip — feature not available on this target by design | n/a |
| `TEST_FLAKY` | Skip by default; run when `/etc/test_run_flaky` exists | skipped |
| `TEST_SLOW` | Skip by default; run when `/etc/test_run_slow` exists | skipped |

The `TEST_DISABLED` vs `TEST_UNSUPPORTED` split matters for triage:
- `TEST_DISABLED` flags real pending work (e.g. rv32's orphan-reaping
  leak or rv32 `clock_gettime` returning -EINVAL for the test's
  clock_ids). The summary's `skipped` count is the to-do list.
- `TEST_UNSUPPORTED` flags by-design gaps that won't be fixed (8086 has
  no FPU; pcxt uses elf16 not elf.c; m68k-only binaries on non-m68k
  targets).  The `n/a` count is informational only.

Platform-specific tests (`test_x68k`, `test_h68k_dos`) are `TEST_UNSUPPORTED`
on non-m68k and `TEST_ENABLED` on m68k via `#if defined(__m68k__)`.

**Runtime filter**

If the file `/etc/test_filter` exists in the romfs, only tests whose path
contains the filter string (substring match) are run.  All others are
silently skipped (not counted in totals).

**Flaky/slow opt-in**

If the file `/etc/test_run_flaky` exists in the romfs, `TEST_FLAKY` tests
run instead of being skipped.

If the file `/etc/test_run_slow` exists in the romfs, `TEST_SLOW` tests
run instead of being skipped.

Both files are created automatically by `scripts/run.sh` via a temporary
overlay directory baked into romfs at build time (see
[`--filter`, `--flaky`, and `--slow`](#filter-flaky-and-slow) below).

### Files

| File | Tests |
|------|-------|
| `test_exec.c` | ELF loading, XIP, GOT relocation |
| `test_elf.c` | ELF32 header validation, segment extraction, `.got` lookup |
| `test_vfork.c` | `vfork` + `execve` + `waitpid` |
| `test_pipe.c` | Pipe creation, read/write, EOF |
| `test_brk.c` | Heap growth via `brk()` |
| `test_fd.c` | `dup`, `dup2`, `close`, redirection |
| `test_signal.c` | Signal handler install + delivery |
| `test_poll.c` | `ppoll` syscall |
| `test_sleep_intr.c` | Process lifecycle, exit codes |
| `test_orphan.c` | Orphan reparenting to init |
| `test_fault.c` | CPU fault handlers (illegal insn, div-by-zero) |
| `test_id.c` | `getpid`, `getppid`, `setpgid`, `getpgid`, `setsid` |
| `test_fs.c` | Filesystem operations (open, read, readdir, stat) |
| `test_rw.c` | File read/write on writable filesystems |
| `test_time.c` | `nanosleep` behavior and error paths |
| `test_iov.c` | `readv`, `writev` scatter/gather I/O |
| `test_stat.c` | `stat`, `getdents` on romfs |
| `test_tmpfs.c` | tmpfs create, write, read, unlink, multi-page I/O (disabled on rv32) |
| `test_ufs.c` | UFS write+read (pcxt only; disabled on romfs-root targets) |
| `test_float.c` | FPU register preservation across context switch (disabled on m68k) |
| `test_signal_float.c` | FPU register preservation across signal delivery (disabled on m68k) |
| `test_x68k.c` | Human68k subsystem (X-format `.x` execution) |
| `test_cpm.c` | CP/M subsystem integration (`.COM` exec, BDOS bridge, signals, file I/O; disabled on rv32) |
| `test_sos.c` | S-OS SWORD subsystem integration (disabled on rv32) |
| `test_msdos.c` | MS-DOS subsystem integration (pcxt only; disabled on all other targets) |
| `test_trace.c` | `ptrace` exec + PPAP syscall trace integration (FLAKY; run with `--flaky`) |
| `test_pdb.c` | `pdb` scripted smoke and command coverage (`TEST_SLOW` on m68k; use `--slow`) |
| `test_pdb_arm_disas.c` | ARM-only `pdb disas` smoke (built in `/bin/`, not in default `runtests`) |
| `test_h68k_dos.c` | Human68k DOS bridge integration via R-format test binaries |
| `test_libc.c` | PPAP libc surface — printf width/zero-pad, strtol/strtoul, ctype, qsort/bsearch, FILE streams, strftime, setjmp/longjmp (UNSUPPORTED on ia16/xtensa — no setjmp.S) |

### Build system

User tests are built by `cmake/user.cmake` as custom commands, controlled
by the `PPAP_TESTS` CMake option. The build system:

- Cross-compiles each `test_*.c` from `tests/user/`
- Builds Human68k R-format user tests from `tests/user/r68k/` on all targets
- Links with `crt0.o` + `syscall.o` (arch-specific SVC stubs)
- Installs to romfs `/bin/`
- Compiler flags: `-fPIC -msingle-pic-base -mpic-register=r9`
  (ARM) or `-msep-data` (m68k)

### Adding a new user-space test

1. Create `tests/user/test_foo.c`:
   ```c
   #include "utest.h"
   int main(void) {
       UT_ASSERT(1, "sanity");
       UT_SUMMARY("test_foo");
   }
   ```
2. Add `test_foo` to the `USER_TESTS` list in `cmake/user.cmake`
3. Add an entry to the test list in `tests/user/runtests.c`:
   ```c
   tests[t++] = (test_entry_t){ "/bin/test_foo", TEST_ENABLED };
   ```
   For per-target gating, put the ifdef inside the struct initialiser:
   ```c
   tests[t++] = (test_entry_t){ "/bin/test_foo",
   #if defined(__riscv)
       TEST_DISABLED      /* fix pending */
   #elif defined(__ia16__)
       TEST_UNSUPPORTED   /* feature n/a here by design */
   #else
       TEST_ENABLED
   #endif
   };
   ```
   Use `TEST_FLAKY` for tests that are known to be unreliable and should
   be skipped in CI.
4. Build with `--test` and run

### Constraints for user-space test code

- **PPAP libc is available.** Tests can `#include <stdio.h>`,
  `<stdlib.h>`, `<string.h>`, `<setjmp.h>`, etc.  Headers come from
  `src/user/include/`; the implementation lives under `src/user/lib/`
  and is linked into every test binary via the same path as user
  apps.  See `tests/user/test_libc.c` for a worked example.  Some
  facilities are arch-conditional — `setjmp` is available on ARM /
  m68k / RISC-V (each arch ships a `user/setjmp.S`) but not on ia16
  or xtensa, so `test_libc` is `TEST_UNSUPPORTED` there.
- **No division on m68k.** GCC emits `__divsi3` calls; use
  subtraction loops (see `ut_print_int` in `utest.h`).
- **No static pointer arrays.** PIC relocation only fixes GOT entries.
  Initialise pointer arrays at runtime, or use a `switch` statement
  returning string literals (which are GOT-resolved per call site).
- **Use `vfork` + `execve`, not `fork`.** PPAP has no MMU; `vfork`
  shares the parent's address space. The child must immediately
  `execve` or `_exit` — do not modify parent data or trigger faults.

### Known coverage gaps (as of 2026-04-18)

Current user-space tests are a solid regression baseline, but subsystem
coverage is not exhaustive yet.

- **`ioctl` wrapper is untested from userland.**
  `src/user/syscall.h` declares `ioctl()`, but no `tests/user/test_*.c`
  currently calls it.
  This leaves tty/device control behavior mostly validated only indirectly
  (through higher-level programs), not with explicit syscall assertions.
- **`test_m68k_emu.c` is present but not in the default on-target suite.**
  The source exists in `tests/user/`, but it is not listed in
  `cmake/user.cmake` `USER_TESTS` or in `tests/user/runtests.c`.
  As a result, regressions in m68k-eCPU user process execution can be missed
  unless the test is run manually.
- **Subsystem ptrace mode is not covered.**
  `test_trace.c` validates `PPAP_TRACE_MODE_PPAP_SYSCALL`; there is no
  user test enabling `PPAP_TRACE_MODE_SUBSYS_CALL` to validate Human68k/CP/M
  subsystem enter/exit trace events.
  In practice, this means ABI-tagged subsystem events (`H68K_DOS`, `H68K_IOCS`,
  `CPM_BDOS`, `CPM_BIOS`) are implemented but not CI-verified end-to-end.
- **Human68k DOS bridge is partially covered.**
  `human68k_dos_dispatch()` implements 56 DOS function IDs; current
  `tests/user/r68k/test_dos_*.S` coverage is 24 IDs. Missing areas include
  several console/input calls, wildcard file search (`_FILES`/`_NFILES`),
  handle duplication (`_DUP`/`_DUP2`), and metadata/update paths
  (`_FILEDATE`, `_SETDATE`, `_SETTIME`, etc.).
  Current R-format tests are strongest on basic process/memory/file/dir flows,
  but weaker on "DOS utility" behaviors and less common compatibility APIs.
- **Human68k IOCS dispatch has no dedicated user test binary.**
  IOCS handlers are implemented, but the current user suite does not have a
  focused IOCS test equivalent to the DOS R-format set.
  This is a notable blind spot for console and system-service compatibility.
- **CP/M BDOS bridge is partially covered.**
  `cpm_bdos_dispatch()` implements 38 function IDs; `test_cpm.c` currently
  targets a core subset (about 16 IDs: 0, 1, 2, 6, 9, 12, 14, 15, 16,
  20, 21, 22, 24, 25, 26, 32). Uncovered areas include search first/next,
  random-record variants, and several disk/attribute vector functions.
  CP/M test coverage is therefore good for bootstrapping and basic file I/O,
  but not yet complete for directory iteration and random-record compatibility.
- **`qemu_rv32`: kernel tests disabled, user 16/16 pass.**
  Kernel tests are disabled because blkdev/VFAT tests expect a FAT32
  ramblk that rv32 does not provide; the 18 failures corrupt state and
  crash init startup.
  User: 16 tests pass (vfork, pipe, fd, poll, id, rw, iov, stat, float,
  libc, exec, brk, fs, signal, sleep_intr, signal_float).  Seven are
  still `TEST_DISABLED` on RISC-V with per-test comments in
  `tests/user/runtests.c`: `test_elf` (ELF parser rejects inputs with
  -ENOEXEC), `test_fault` (kernel trap handler terminates the whole
  system on illegal-instruction instead of delivering SIGILL),
  `test_orphan` (leak-check assertion at line 105 fails — more than
  12 KB of user_pages unaccounted for after orphans exit),
  `test_time` (`clock_gettime` returns -EINVAL), `test_tmpfs` (line
  115 errno mismatch with corrupted summary counters), `test_cpm` /
  `test_sos` (Z80 eCPU path takes a load-access fault early and
  brings the kernel down).
- **`xtensa_cc` (CardComputer hardware): 13 user tests pass cleanly
  after the vfork user-stack-page leak fix.**
  Ten tests are `TEST_DISABLED` / `TEST_UNSUPPORTED` on `__xtensa__`
  with per-test comments in `tests/user/runtests.c`:
  * `test_elf` — ELF32 parser rejects inputs with -ENOEXEC, same
    failure mode as rv32.
  * `test_signal`, `test_sleep_intr` — CC-3 signal-delivery stub:
    `deliver_signal()` does not yet build a sigreturn frame, so
    SIGUSR1 handlers never run.
  * `test_iov` — exits 1; root cause not yet investigated.
  * `test_float`, `test_signal_float` — `TEST_UNSUPPORTED`: xtensa
    user-space is built with `-mabi=call0` and no coprocessor enable,
    so any FP instruction triggers a Guru Meditation Coprocessor
    exception that ESP-IDF's panic handler converts into a chip
    reboot.
  * `test_env` — `setenv` / `unsetenv` paths return -1 at
    test_env.c:91, :93, :97; root cause not yet investigated.
  * `test_tmpfs` — write returns 0 instead of length; ENOSPC handling
    mismatches at multiple line offsets.
  * `test_cpm` — 50+ internal failures, most are file-open returning
    -1 (`/bin/hello.com` etc.); either the CP/M subsystem loader is
    broken on xtensa or the `.com` companion binaries aren't staged
    into the xtensa_cc romfs.
  * `test_sos` — exit 127, binary load failing before main() runs.

  Earlier passes of this document described an "OOM cascade from
  page-pool fragmentation" as the explanation for ~10 cascade-victim
  failures.  The actual cause was a per-vfork page leak in
  `sys_vfork()`'s xtensa branch (allocated user-stack page never
  recorded on the child PCB, never freed); fixed in commit
  `6acdf23f`.  With the leak plugged, six tests that previously
  reported `FAIL exit 127 (OOM)` now run cleanly (`test_id`,
  `test_fs`, `test_rw`, `test_stat`, `test_time`, `test_orphan`), and
  the remaining real failures are visible without OOM-cascade noise.

  None of the four newly-`TEST_DISABLED` real failures (`test_env`,
  `test_tmpfs`, `test_cpm`, `test_sos`) were observable before the
  leak fix — all were masked by OOM-cascade exit-127s.  They are
  recorded here as a baseline for the remaining xtensa_cc subsystem
  and loader work.

  The user-space loader is still RAM-only.  At boot `/proc/meminfo`
  reports ~72 KB free (≈18 pages); the experimental `user_xip.ld` /
  `.xip` / `.xipfix` build variants exist but the runtime loader
  does not consume them yet.  Wiring those in remains a follow-up in
  `docs/proposals/cardcomputer_port.md` — independent of the leak
  fix, it would still cut per-`exec` page use significantly.

  **Hardware-flash troubleshooting note (host USB):** repeated
  docker-passthrough flashes of `xtensa_cc` occasionally leave the
  host's xHCI controller in a confused state where the device stops
  appearing in `lsusb` and `/dev/ttyACM*` disappears.  Rebooting
  fixes it, but a faster escape hatch is to unbind/rebind the xHCI
  PCI device:

  ```sh
  lspci -nn | grep -iE 'usb|xhci'      # find xHCI PCI ID (e.g. 0000:05:00.3)
  echo '0000:05:00.3' | sudo tee /sys/bus/pci/drivers/xhci_hcd/unbind
  sleep 1
  echo '0000:05:00.3' | sudo tee /sys/bus/pci/drivers/xhci_hcd/bind
  ```

  Anything else on that controller (keyboard, mouse, hubs) briefly
  disappears and re-enumerates during the 1-second window.  The
  longer-term mitigation is to move esptool out of docker entirely
  so the host CDC-ACM handle never crosses a namespace boundary;
  tracked in `docs/proposals/cardcomputer_port.md`.

  Captured serial output is persisted to `$BUILD_DIR/test_output.log`
  during `--test xtensa_cc` runs, so partial output survives a
  mid-run disconnect.
- **`pdb` scripted coverage is architecture-asymmetric.**
  `test_pdb` has 170/367 failures on m68k and is marked `TEST_DISABLED` there.
  On ARM it is `TEST_SLOW` (base runner) / `TEST_ENABLED` (extended runner).
  ARM also has a dedicated `test_pdb_arm_disas` binary for disassembly smoke.

#### Suggested follow-up tests

1. Add `tests/user/test_ioctl.c` with basic tty/device `ioctl` happy/error
   paths and include it in `USER_TESTS` and `runtests`.
2. Enable `tests/user/test_m68k_emu.c` in the default suite.
3. Extend `tests/user/test_trace.c` to exercise `PPAP_TRACE_MODE_SUBSYS_CALL`
   against one Human68k and one CP/M child.
4. Add one new Human68k R-format test focused on currently uncovered DOS calls
   (for example `_FILES`/`_NFILES` plus `_FILEDATE`).
5. Add one CP/M `.COM` test focused on uncovered BDOS functions (for example
   search first/next and at least one random-record operation).
6. Add CPUTEST (another Z80 exerciser) for additional coverage beyond
   ZEXALL.

## Automated on-target testing

### `run.sh --test`

Builds with `PPAP_TESTS=ON`, boots the selected target, and captures
console output until the harness can decide whether the run passed.  QEMU
targets terminate themselves after the test summary; hardware targets use
a host-side serial monitor.

The pass marker is the exact string `ALL TESTS PASSED`.

**QEMU backends:** After printing the test summary, `runtests`
calls `poweroff()` which invokes `SYS_POWEROFF` (0x0B00).  The kernel
routes this to `target_qemu_poweroff()`, which writes to an
architecture-specific QEMU exit device:

| Target | Mechanism | Exit device |
|--------|-----------|-------------|
| `qemu_arm` | ARM semihosting `SYS_EXIT_EXTENDED` | bkpt 0xAB |
| `qemu_m68k` | `virt-ctrl` MMIO halt | 0xFF009004 |
| `qemu_rv32` | `sifive_test` MMIO | 0x100000 |
| `pcxt` | `isa-debug-exit` I/O port | 0x501 |

Kernel panics and unhandled faults also trigger the same exit path via
`kernel_panic_halt(1)`, so QEMU exits immediately on fatal errors
instead of spinning until the external timeout.

**Xtensa hardware backend:** `xtensa_cc` flashes the ESP32-S3, starts a
serial monitor, and stops the host-side test process as soon as it sees
either `ALL TESTS PASSED` or `SOME TESTS FAILED`.  The target does not
need a guest-side quit path; after the marker appears, the board may keep
running while `run.sh` exits based on the captured log.

The external `timeout` in `run.sh` remains as a safety net but should
rarely fire.

- ARM default timeout: 90 seconds
- RISC-V default timeout: 90 seconds
- m68k default timeout: 90 seconds
- m68k with `--slow`: 150 seconds
- pcxt default timeout: 180 seconds
- xtensa_cc default timeout: 180 seconds

**Current test results:**

| Target | Date | Kernel tests | User tests | Total |
|--------|------|--------------|------------|-------|
| `qemu_arm` | 2026-04-18 | 69 pass | 23/23 pass | All pass |
| `qemu_m68k` | 2026-04-18 | 69 pass | 23/23 pass | All pass |
| `qemu_rv32` | 2026-04-18 | Disabled (pre-existing blkdev crash) | 16/16 pass | User pass |
| `pcxt` | 2026-04-18 | N/A (no ktest) | 17/17 pass | All pass |
| `xtensa_cc` | 2026-05-12 | 62 pass, 7 fail | 13/13 pass | User pass |

```bash
./scripts/run.sh --test              # ARM (default)
./scripts/run.sh --test qemu_rv32    # RISC-V
./scripts/run.sh --test qemu_m68k    # m68k
./scripts/run.sh --test pcxt         # PC/XT (i16)
./scripts/run.sh --test xtensa_cc    # Xtensa hardware
```

### pcxt notes

On pcxt, the VGA console is invisible to the QEMU serial capture used
by the test runner. The `runtests` binary redirects stdout to
`/dev/ttyS0` at startup so test output appears on the serial port.
(The redirect is unconditional — on other targets `/dev/ttyS0` open
either succeeds harmlessly or fails and is skipped.)

The pcxt user suite runs 17 of the shared user tests today. The
remaining entries fall into two buckets visible in the summary:
- `skipped` (2): `test_sleep_intr` (nanosleep-interruption path not
  implemented on ia16) and `test_fault` (ia16 routes CPU faults to
  kernel panic).  Both are `TEST_DISABLED`, not `TEST_UNSUPPORTED` —
  they can be lit up when the underlying features land.
- `n/a` (12): tests that exercise features pcxt does not have — no
  FPU, no Z80/CP/M/S-OS subsystems, no setjmp (so `test_libc` is
  unsupported), ELF32 parser n/a (pcxt uses elf16), arch-specific
  ptrace regsets, etc.

### `run.sh --test-extended`

Builds with `PPAP_TESTS=ON` and `PPAP_TESTS_EXTENDED=ON`, runs under QEMU
with a larger timeout budget, and executes `/bin/runtests_ext` as PID 1.

- ARM extended timeout: 90 seconds
- m68k extended timeout: 150 seconds
- m68k extended with `--slow`: 180 seconds

```bash
./scripts/run.sh --test-extended qemu_arm
./scripts/run.sh --test-extended qemu_m68k
```

### `--filter`, `--flaky`, and `--slow`

These flags write config files into a temporary overlay directory that
is merged into romfs at build time, then cleaned up.

```bash
# Run only tests whose path contains "pipe"
./scripts/run.sh --test --filter=pipe

# Also run tests marked TEST_FLAKY
./scripts/run.sh --test --flaky

# Also run tests marked TEST_SLOW
./scripts/run.sh --test --slow

# Combine: filter + flaky
./scripts/run.sh --test --filter=pdb --flaky

# Combine: filter + flaky + slow
./scripts/run.sh --test --filter=pdb --flaky --slow
```

`--filter` writes `/etc/test_filter` (the substring to match).
`--flaky` writes `/etc/test_run_flaky` (existence is the signal; content
is ignored).
`--slow` writes `/etc/test_run_slow` (existence is the signal; content
is ignored).  All imply `--build`.

On m68k lanes, enabling `--slow` increases `run.sh` timeout so slow tests
such as `test_pdb` do not fail due to harness timeout alone.

### `test.sh --all`

Full CI pipeline:
1. Host unit tests
2. Build all production targets (ARM and m68k, `PPAP_TESTS=OFF`)
3. Print binary sizes
4. QEMU on-target tests (ARM and m68k, `PPAP_TESTS=ON`)

`./scripts/test.sh --all --extended` (or `--all-extended`) replaces step 4
with extended QEMU lanes (`run.sh --test-extended`).

### Test execution flow

```
boot → kernel init → VFS mount → target_post_mount()
                                   │
                           ┌───────┴───────┐
                           │ PPAP_TESTS=1  │
                           └───────┬───────┘
                                   │
                          ktest_run_all()
                         (kernel tests)
                                   │
                          sched_start()
                                   │
                     target_init_path() = "/bin/runtests"
                                   │
                          runtests (PID 1)
                            ├── [T+0.00] test_exec
                            ├── [T+0.3x] test_elf
                            ├── test_vfork
                            ├── test_fault
                            ├── test_pipe
                            ├── test_brk
                            ├── test_fd
                            ├── test_signal
                            ├── test_poll
                            ├── test_sleep_intr
                            ├── test_orphan
                            ├── test_id
                            ├── test_fs
                            ├── test_rw
                            ├── test_time
                            ├── test_iov
                            ├── test_stat
                            ├── test_tmpfs
                            ├── test_x68k   (ENABLED on m68k, DISABLED on ARM)
                            ├── test_h68k_dos (ENABLED on m68k, DISABLED on ARM)
                            ├── test_cpm
                            ├── test_sos
                            ├── test_libc   (UNSUPPORTED on ia16/xtensa)
                            ├── test_trace  (FLAKY; run with --flaky)
                            └── test_pdb    (SLOW; run with --slow)
                                   │
               "ALL TESTS PASSED" or "SOME TESTS FAILED"
                                   │
                    target-specific harness exit
                  (QEMU poweroff or serial monitor)
                                   │
                   run.sh --test checks output
```

Extended lane (`--test-extended`) uses the same flow except
`target_init_path()` resolves to `/bin/runtests_ext`.

## Build flags reference

| Flag | Where | Effect |
|------|-------|--------|
| `--test` | `./scripts/run.sh` / `build.sh` | Sets `PPAP_TESTS=ON` in CMake |
| `--test-extended` | `./scripts/run.sh` / `build.sh` | Sets `PPAP_TESTS=ON` and `PPAP_TESTS_EXTENDED=ON` |
| `--filter=<pattern>` | `./scripts/run.sh` | Writes `/etc/test_filter` into romfs overlay; implies `--build` |
| `--flaky` | `./scripts/run.sh` | Writes `/etc/test_run_flaky` into romfs overlay; implies `--build` |
| `--slow` | `./scripts/run.sh` | Writes `/etc/test_run_slow` into romfs overlay; implies `--build` |
| `PPAP_TESTS=ON` | CMake option | Compiles `ktest.c` into kernel; defines `PPAP_TESTS=1` C macro; enables user test builds |
| `PPAP_TESTS_EXTENDED=ON` | CMake option | Defines `PPAP_TESTS_EXTENDED=1`; selects `/bin/runtests_ext` as init path |
| `PPAP_TESTS=1` | C preprocessor | Guards `ktest_run_all()` call in `target_post_mount()`; selects `/bin/runtests` as init path |
| `PPAP_TESTS_EXTENDED=1` | C preprocessor | Selects `/bin/runtests_ext` as init path |
| `PPAP_TESTS=ON` | `cmake/user.cmake` | Builds test binaries and installs to romfs |
