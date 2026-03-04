# Phase 7: Target Support Packages — Detailed Plan

**PicoPiAndPortable Development Roadmap**
Estimated Duration: 2 weeks
Prerequisites: Phase 6 (musl + busybox) complete
**Status: Complete**

---

## Goals

Split target-specific code (drivers, pin definitions, boot sequences, linker
scripts, kernel entry points) into per-target directories and define three
official build targets: `qemu_arm`, `pico1`, and `pico1calc`.  After this phase
the same kernel source tree produces three independent binaries from a single
`ninja` invocation, with no `#ifdef` guards scattered through shared kernel
code.

A CMake build option **`PPAP_TESTS`** (default OFF) enables kernel integration
tests and the userland test suite for any target.  When enabled, the target
runs `ktest_run_all()` after fstab mount and execs `/bin/runtests` as PID 1
instead of `/sbin/init`.  This replaces the old `main_qemu.c` test harness
with a mechanism that works on any target (including future ones).

**Exit Criteria (all must pass before moving to Phase 8):**
- Three CMake targets: `ppap_qemu_arm`, `ppap_pico1`, `ppap_pico1calc`
- Each target produces a working binary (ELF + UF2 for hardware targets)
- Per-target directories: `src/target/qemu_arm/`, `src/target/pico1/`, `src/target/pico1calc/`
- Target abstraction header (`src/target/target.h`) provides a uniform init API
- No `#ifdef PPAP_QEMU` (or similar) in shared kernel code (`src/kernel/`)
- `ppap_pico1` boots to ash shell on the official Raspberry Pi Pico (romfs-only, no SD)
- `ppap_pico1calc` boots to ash shell on PicoCalc (full filesystem with SD)
- `ppap_qemu_arm` boots to ash shell under QEMU mps2-an500
- `-DPPAP_TESTS=ON` builds any target with kernel + userland tests enabled
- `-DPPAP_TESTS=OFF` (default) builds without any test code or test binaries
- Linker scripts are per-target (not shared between incompatible targets)
- `ninja ppap_qemu_arm ppap_pico1 ppap_pico1calc` builds all three from one build directory
- QEMU CI smoke test (`scripts/qemu-test.sh`) builds with `PPAP_TESTS=ON` and passes
- QEMU interactive test (`scripts/qemu.sh`) builds default and boots to ash shell
- Hardware test: `ppap_pico1.uf2` flashed to official Pico, ash shell works over UART
- Hardware test: `ppap_pico1calc.uf2` flashed to PicoCalc, ash shell + SD access works

---

## Source Tree After Phase 7

```
src/
  target/
    target.h                    ✓ New: target abstraction API (function declarations)
    qemu_arm/
      CMakeLists.txt            ✓ New: ppap_qemu_arm target definition
      target_qemu_arm.c         ✓ New: target hooks for QEMU
      drivers/
        uart_qemu.c             ← moved from src/drivers/uart_qemu.c
    pico1/
      CMakeLists.txt            ✓ New: ppap_pico1 target definition
      target_pico1.c            ✓ New: target_early_init(), target_late_init() for Pico
      pico1.h                   ✓ New: Pico pin definitions (UART0 GP0/GP1)
    pico1calc/
      CMakeLists.txt            ✓ New: ppap_pico1calc target definition
      target_pico1calc.c        ✓ New: target_early_init(), target_late_init() for PicoCalc
      pico1calc.h               ← merged + renamed from src/board/picocalc.h
  drivers/
    uart.c                      (existing — RP2040 PL011, shared by pico1 + pico1calc)
    uart.h                      (existing)
    clock.c                     (existing — PLL init, shared by pico1 + pico1calc)
    clock.h                     (existing)
    spi.c                       (existing — SPI0 driver, pico1calc only)
    spi.h                       (existing)
    sd.c                        (existing — SD card, pico1calc only)
    sd.h                        (existing)
  boot/
    startup.S                   (existing — shared by all targets)
    stage1.S                    (existing — shared by pico1 + pico1calc)
  kernel/
    main.c                      (modified — shared kmain(), calls target_*() hooks)
    main_qemu.c                 ← REMOVED (init logic → target/qemu_arm/, tests → src/test/)
    ...                         (all other kernel code unchanged)
tests/
  kernel/
    ktest.c                     ✓ Kernel integration tests (extracted from main_qemu.c)
    ktest.h                     ✓ ktest_run_all() declaration
  host/
    CMakeLists.txt              ✓ Host-native unit test build
    test_kmem.c                 ✓ kmem unit tests
    test_fd.c                   ✓ fd unit tests
    test_elf.c                  ✓ ELF parser unit tests
    stubs/                      ✓ UART/tty/xip stubs for host builds
  user/
    test_vfork.c ... runtests.c ✓ User-space test programs (moved from user/)
  config.h                      (existing — shared constants)
ldscripts/
  qemu.ld                       (existing — unchanged)
  pico1.ld                      ✓ New: RP2040 layout, 2 MB flash, no I/O buffer
  pico1calc.ld                  ← renamed from ppap.ld (RP2040 layout, 16 MB flash)
scripts/
  qemu.sh                       (existing — updated for ppap_qemu_arm binary name)
  qemu-test.sh                  (existing — updated to use PPAP_TESTS=ON build)
  test_all_targets.sh           ✓ New: build + smoke test all 3 targets
cmake/
  kernel_sources.cmake          ✓ New: KERNEL_COMMON_SOURCES list (included by each target CMakeLists)
CMakeLists.txt                  (modified — project setup, shared pipelines, add_subdirectory() per target)
```

**Overlay convention:** Each target directory is a sparse overlay mirroring
the `src/` layout.  Only files that differ from the shared tree are present.
CMake picks the target-specific file instead of (or in addition to) the
shared one.  For example, the QEMU target overrides the UART driver by
placing its version at `src/target/qemu_arm/drivers/uart_qemu.c`.

Each target also has its own `CMakeLists.txt` that defines its
`add_executable()`, link options, compile definitions, and SDK-specific
bits.  The top-level `CMakeLists.txt` handles project setup, Pico SDK
init, and shared pipelines (romfs, tools, user binaries), then includes
each target via `add_subdirectory()`.  Adding a new target = new directory
+ one `add_subdirectory()` line in the root.

A future x68k target might look like:

```
src/target/x68k/
  CMakeLists.txt              # ppap_x68k target (no Pico SDK needed)
  target_x68k.c               # target init hooks
  drivers/
    uart_x68k.c               # MFP UART driver
  boot/
    startup_x68k.S            # 68000 vector table + reset handler
```

---

## Week 1: Target Abstraction and Refactoring

### Step 1 — Target Abstraction API (`src/target/target.h`)

Define a minimal target abstraction that each target implements.  The kernel
calls these hooks instead of directly invoking hardware-specific functions.

**Design choices:**
- Function-pointer vtable rejected (adds indirection + flash overhead on M0+).
  Instead, use link-time selection: each target provides the same function
  symbols, and CMake links the correct `target_<name>.c`.
- Keep the API surface small: 4 hooks + 1 query function.  Two of the hooks
  (`target_post_mount`, `target_init_path`) are test-aware via `#ifdef
  PPAP_TESTS` in each target's implementation — this keeps the `#ifdef` in
  target-specific code, never in shared kernel code.  Targets that need more
  (e.g., PicoCalc SD detect) expose them through their own headers.
- `target.h` is the *only* header shared kernel code includes for
  target-specific behavior.  Individual target headers (`pico1.h`,
  `picocalc.h`) are included only from their own `target_*.c` files.
- **Overlay layout:** each target directory mirrors the `src/` subtree
  structure.  Target-specific drivers live under `<target>/drivers/`,
  target-specific boot code under `<target>/boot/`, etc.  This makes it
  immediately clear what each target overrides and scales naturally to
  future targets (e.g., x68k) that may replace many more shared files.

**API (`src/target/target.h`):**

```c
#ifndef PPAP_TARGET_H
#define PPAP_TARGET_H

#include <stdint.h>

/* Target capability flags — returned by target_caps() */
#define TARGET_CAP_SD       (1u << 0)   /* Has SD card slot           */
#define TARGET_CAP_SPI      (1u << 1)   /* Has SPI bus for peripherals */
#define TARGET_CAP_CORE1    (1u << 2)   /* Dual-core (Core 1 usable)  */
#define TARGET_CAP_REALUART (1u << 3)   /* PL011 UART (not CMSDK)     */

/*
 * target_early_init() — called first in kmain(), before mm_init().
 *
 * Responsibilities (target-dependent):
 *   - UART console init (so uart_puts() works immediately)
 *   - Clock PLL init (RP2040 targets only)
 *   - UART baud rate update after PLL
 *   - SPI bus init (PicoCalc only)
 *
 * After this call: UART console is operational, system clock is final.
 */
void target_early_init(void);

/*
 * target_late_init() — called after VFS/blkdev init, before sched_start().
 *
 * Responsibilities (target-dependent):
 *   - SD card detection and initialization
 *   - Block device registration (mmcblk0)
 *   - UART switch to IRQ mode
 *   - MPU configuration
 *   - Core 1 launch
 *   - RAM block device setup (QEMU only)
 *
 * After this call: all block devices are registered, ready for fstab mount.
 */
void target_late_init(void);

/*
 * target_post_mount() — called after VFS + fstab mount, before sched_start().
 *
 * Default build (PPAP_TESTS off): empty (no-op).
 * Test build  (PPAP_TESTS on):   runs kernel integration tests via ktest_run_all().
 *
 * Each target implements this with #ifdef PPAP_TESTS in its own .c file.
 */
void target_post_mount(void);

/*
 * target_init_path() — returns the path to exec as PID 1.
 *
 * Default build (PPAP_TESTS off): "/sbin/init" (busybox ash shell).
 * Test build  (PPAP_TESTS on):   "/bin/runtests" (automated test runner).
 *
 * Each target implements this with #ifdef PPAP_TESTS in its own .c file.
 */
const char *target_init_path(void);

/*
 * target_caps() — returns a bitmask of target capabilities.
 *
 * Used by shared code to conditionally skip SD-dependent steps
 * (e.g., fstab skips VFAT/loopback entries when TARGET_CAP_SD is absent).
 */
uint32_t target_caps(void);

#endif /* PPAP_TARGET_H */
```

**Integration point:** `main.c` becomes:

```c
void kmain(void) {
    target_early_init();    /* UART + clock + SPI */
    mm_init();
    proc_init();
    vfs_init();
    file_pool_init();
    blkdev_init();
    loopback_init();
    target_late_init();     /* SD card + IRQ UART + MPU + Core 1 */

    /* Mount romfs at / */
    vfs_mount("/", &romfs_ops, MNT_RDONLY, __romfs_start);

    /* Parse and mount fstab */
    fstab_entry_t fstab[FSTAB_MAX_ENTRIES];
    int nfstab = fstab_parse(fstab, FSTAB_MAX_ENTRIES);
    if (nfstab > 0)
        fstab_mount_all(fstab, nfstab);

    target_post_mount();    /* kernel integration tests (PPAP_TESTS=ON only) */

    /* Wire fd 0/1/2, launch init, start scheduler */
    fd_stdio_init(&proc_table[0]);
    /* ... exec target_init_path() ... */
    proc_table[0].stack_page = page_alloc();
    sched_start();
    for (;;) __asm__ volatile ("wfi");
}
```


### Step 2 — Per-Target Implementations

Create the three `target_*.c` files, each implementing the `target.h` API.
Each target includes `#ifdef PPAP_TESTS` blocks for `target_post_mount()`
and `target_init_path()` — these are in target-specific code, not shared
kernel code, so they don't violate the zero-ifdef principle.

**`src/target/pico1calc/target_pico1calc.c`:**

```c
#include "../target.h"
#include "picocalc.h"
#include "drivers/uart.h"
#include "drivers/clock.h"
#include "drivers/spi.h"
#include "drivers/sd.h"
#include "mm/mpu.h"
#include "smp.h"

void target_early_init(void) {
    uart_init_console();        /* 115200 bps @ 12 MHz XOSC */
    uart_puts("PicoPiAndPortable booting... [pico1calc]\n");
    uart_flush();
    clock_init_pll();           /* 133 MHz */
    uart_reinit_133mhz();
    uart_puts("System clock: 133 MHz\n");
    spi_init(400000);           /* SPI0 @ 400 kHz for SD init */
    uart_puts("SPI0: initialised at 400 kHz\n");
}

void target_late_init(void) {
    int rc = sd_init();
    if (rc == 0)
        uart_puts("SD: card initialised, mmcblk0 registered\n");
    else
        uart_puts("SD: no card or init failed (skipping)\n");

    uart_flush();
    uart_init_irq();
    uart_puts("UART: switched to interrupt-driven mode\n");
    mpu_init();
    core1_launch(core1_io_worker);
}

void target_post_mount(void) {
#ifdef PPAP_TESTS
    ktest_run_all();
#endif
}

const char *target_init_path(void) {
#ifdef PPAP_TESTS
    return "/bin/runtests";
#else
    return "/sbin/init";
#endif
}

uint32_t target_caps(void) {
    return TARGET_CAP_SD | TARGET_CAP_SPI | TARGET_CAP_CORE1 | TARGET_CAP_REALUART;
}
```

**`src/target/pico1/target_pico1.c`:**

```c
#include "../target.h"
#include "pico1.h"
#include "drivers/uart.h"
#include "drivers/clock.h"
#include "mm/mpu.h"
#include "smp.h"

void target_early_init(void) {
    uart_init_console();
    uart_puts("PicoPiAndPortable booting... [pico1]\n");
    uart_flush();
    clock_init_pll();
    uart_reinit_133mhz();
    uart_puts("System clock: 133 MHz\n");
    /* No SPI init — pico1 has no SD card slot */
}

void target_late_init(void) {
    /* No SD card to initialize */
    uart_flush();
    uart_init_irq();
    uart_puts("UART: switched to interrupt-driven mode\n");
    mpu_init();
    core1_launch(core1_io_worker);
}

void target_post_mount(void) {
#ifdef PPAP_TESTS
    ktest_run_all();
#endif
}

const char *target_init_path(void) {
#ifdef PPAP_TESTS
    return "/bin/runtests";
#else
    return "/sbin/init";
#endif
}

uint32_t target_caps(void) {
    return TARGET_CAP_CORE1 | TARGET_CAP_REALUART;
    /* No TARGET_CAP_SD, no TARGET_CAP_SPI */
}
```

**`src/target/qemu_arm/target_qemu_arm.c`:**

```c
#include "../target.h"
#include "drivers/uart.h"
#include "blkdev/ramblk.h"

/* Linker-provided FAT32 test image (from fatimg_data.S) */
extern const uint8_t __fatimg_start[];
extern const uint8_t __fatimg_end[];

void target_early_init(void) {
    uart_init_console();        /* CMSDK UART, 25 MHz fixed */
    uart_puts("PicoPiAndPortable booting... [qemu_arm]\n");
    /* No PLL, no SPI */
}

void target_late_init(void) {
    /* Register RAM-backed block device from embedded FAT32 image */
    uint32_t fatimg_size = (uint32_t)(__fatimg_end - __fatimg_start);
    if (fatimg_size > 0)
        ramblk_register(__fatimg_start, fatimg_size);
    /* No IRQ UART switch, no MPU, no Core 1 on QEMU */
}

void target_post_mount(void) {
#ifdef PPAP_TESTS
    ktest_run_all();
#endif
}

const char *target_init_path(void) {
#ifdef PPAP_TESTS
    return "/bin/runtests";
#else
    return "/sbin/init";
#endif
}

uint32_t target_caps(void) {
    return 0;  /* No SD, no SPI, no Core 1, no PL011 */
}
```

**`src/test/ktest.c`** (extracted from `main_qemu.c`):

Contains all kernel integration test functions currently in `main_qemu.c`:
`vfs_integration_test()`, `pipe_integration_test()`, `dup_integration_test()`,
`brk_integration_test()`, `signal_integration_test()`, `blkdev_integration_test()`,
`vfat_integration_test()`, `loopback_integration_test()`, `tmpfs_integration_test()`,
`ufs_integration_test()`, `fstab_integration_test()`.

This file is **only compiled when `PPAP_TESTS=ON`** — CMake conditionally
adds it to each target's source list.

```c
/* src/test/ktest.h */
#ifndef PPAP_KTEST_H
#define PPAP_KTEST_H
void ktest_run_all(void);
#endif

/* src/test/ktest.c — ktest_run_all() calls each test suite, prints summary */
void ktest_run_all(void) {
    int pass = 0, fail = 0;

    vfs_integration_test(&pass, &fail);
    pipe_integration_test(&pass, &fail);
    dup_integration_test(&pass, &fail);
    brk_integration_test(&pass, &fail);
    signal_integration_test(&pass, &fail);
    blkdev_integration_test(&pass, &fail);
    vfat_integration_test(&pass, &fail);
    loopback_integration_test(&pass, &fail);
    tmpfs_integration_test(&pass, &fail);
    ufs_integration_test(&pass, &fail);
    fstab_integration_test(&pass, &fail);

    uart_puts(fail == 0 ? "ALL KERNEL TESTS PASSED\n"
                        : "KERNEL TESTS FAILED\n");
}
```

**`src/target/pico1/pico1.h`:**

```c
#ifndef PPAP_TARGET_PICO1_H
#define PPAP_TARGET_PICO1_H

/* Official Raspberry Pi Pico — RP2040, 2 MB flash, no SD card.
 * UART0: GP0 (TX) / GP1 (RX) — same as PicoCalc.
 * No SPI peripherals used by PPAP. */

#define PICO1_UART0_TX      0    /* GP0 */
#define PICO1_UART0_RX      1    /* GP1 */

/* On-board LED (active high) — useful for debug heartbeat */
#define PICO1_LED            25   /* GP25 */

#endif /* PPAP_TARGET_PICO1_H */
```


### Step 3 — Unified `main.c` (Remove `main_qemu.c`)

Merge the two kernel entry points into a single `main.c` that delegates all
target-specific work to the target hooks.

**What changes in `main.c`:**
- Replace direct calls to `uart_init_console()`, `clock_init_pll()`,
  `spi_init()`, `sd_init()`, etc. with `target_early_init()`
- Replace direct calls to `uart_init_irq()`, `mpu_init()`,
  `core1_launch()`, `sd_init()` with `target_late_init()`
- Add `target_post_mount()` call after fstab mount (runs tests if PPAP_TESTS=ON)
- Use `target_init_path()` when exec'ing PID 1 (instead of hardcoded path)
- Remove all `#include` of hardware-specific headers (clock.h, spi.h, sd.h)
- Add `#include "target/target.h"`
- Delete `main_qemu.c` entirely — init logic moves to `target_qemu_arm.c`,
  test functions move to `src/test/ktest.c`

**Kernel integration tests are no longer in shared kernel code.**  The test
functions that were in `main_qemu.c` are extracted into `src/test/ktest.c`
and only compiled when `PPAP_TESTS=ON` (CMake adds it conditionally).
The `#ifdef PPAP_TESTS` guard in each target's `target_post_mount()`
and `target_init_path()` is in target-specific code, not shared kernel code.

**QEMU ramblk/FAT32/UFS test logic:** The QEMU target's `target_late_init()`
handles registering the RAM block device.  The UFS/ramblk source files are
included only in the QEMU CMake target's source list (no `#ifdef` needed).


### Step 4 — Remove `#ifdef PPAP_QEMU` from Shared Kernel Code

Currently `fstab.c` has `#ifdef PPAP_QEMU` guards around UFS support.
This violates the "no ifdefs in shared code" principle.

**Solution:** Always compile UFS support into the fstab mount logic.  UFS
is a standard filesystem that should work on any target (it will be used on
pico1calc for `/usr`, `/home`, `/var`).  The current `#ifdef` exists only
because UFS was developed/tested on QEMU first, but it's not fundamentally
QEMU-specific.

**Changes to `fstab.c`:**
- Remove `#ifdef PPAP_QEMU` / `#endif` around UFS include and mount logic
- UFS support is now unconditional — the fstab parser mounts UFS images
  on any target where the underlying block device exists
- If a VFAT mount fails (no SD card on pico1), dependent UFS loopback
  mounts are skipped naturally (blkdev_find returns NULL)

**Changes to CMakeLists.txt:**
- Add `src/kernel/fs/ufs.c` to the hardware targets too (pico1 and pico1calc)
- The code size increase is ~8 KB in flash — well within budget

This is the **only `#ifdef PPAP_QEMU`** in shared code; removing it
achieves the zero-ifdef goal.

---

## Week 2: Build System, Linker Scripts, and Testing

### Step 5 — pico1 Linker Script (`ldscripts/pico1.ld`)

The official Pico has only 2 MB flash (vs PicoCalc's 16 MB).  The linker
script needs adjustment.

**Differences from pico1calc.ld (née ppap.ld):**

| Region | pico1calc | pico1 | Notes |
|---|---|---|---|
| FLASH_BOOT | 4 KB @ 0x10000000 | 4 KB @ 0x10000000 | Same |
| FLASH_KERNEL | 64 KB @ 0x10001000 | 64 KB @ 0x10001000 | Same |
| FLASH_ROMFS | ~16 MB @ 0x10011000 | ~1.9 MB @ 0x10011000 | 2 MB total flash |
| RAM_KERNEL | 20 KB | 20 KB | Same |
| RAM_PAGES | 204 KB (51 × 4 KB) | 204 KB (51 × 4 KB) | Same |
| RAM_IOBUF | 24 KB | 24 KB | Same (retained for future use) |
| RAM_DMA | 16 KB | 16 KB | Same |

**`pico1.ld`** is a copy of `pico1calc.ld` with one change:

```
FLASH_ROMFS (r) : ORIGIN = 0x10011000, LENGTH = 2M - 0x11000
```

The romfs for pico1 must fit in ~1.9 MB.  A minimal busybox (~300 KB) plus
config files fits comfortably.  The build system should emit a warning if
the romfs image exceeds this limit.

**Rename `ppap.ld` → `pico1calc.ld`** to follow the per-target naming
convention.  Keep the content identical.


### Step 6 — CMakeLists.txt Refactoring

The CMakeLists.txt currently defines `ppap` and `ppap_qemu` in a single
file.  Refactor into a split structure: the top-level file handles project
setup and shared pipelines, a common include defines kernel sources, and
each target has its own CMakeLists.txt.  Three targets are defined:
`ppap_qemu_arm`, `ppap_pico1`, `ppap_pico1calc`.  A top-level CMake option
`PPAP_TESTS` (default OFF) controls whether test code is compiled.

**File layout:**

| File | Responsibility |
|---|---|
| `CMakeLists.txt` | `project()`, `pico_sdk_init()`, shared pipelines (romfs, mkfatimg, mkufs, user binaries), `add_subdirectory()` per target |
| `cmake/kernel_sources.cmake` | `KERNEL_COMMON_SOURCES` list — included by each target's CMakeLists.txt |
| `src/target/<name>/CMakeLists.txt` | `add_executable()`, link options, compile defs, SDK bits for that target (3 targets) |

**`cmake/kernel_sources.cmake`:**

```cmake
# Shared kernel sources — common to all targets.
# Included by each target's CMakeLists.txt.
set(KERNEL_COMMON_SOURCES
    ${CMAKE_SOURCE_DIR}/src/boot/startup.S
    ${CMAKE_SOURCE_DIR}/src/kernel/main.c
    ${CMAKE_SOURCE_DIR}/src/kernel/mm/xip.c
    ${CMAKE_SOURCE_DIR}/src/kernel/mm/page.c
    ${CMAKE_SOURCE_DIR}/src/kernel/mm/kmem.c
    ${CMAKE_SOURCE_DIR}/src/kernel/mm/mpu.c
    ${CMAKE_SOURCE_DIR}/src/kernel/proc/proc.c
    ${CMAKE_SOURCE_DIR}/src/kernel/proc/switch.S
    ${CMAKE_SOURCE_DIR}/src/kernel/proc/sched.c
    ${CMAKE_SOURCE_DIR}/src/kernel/syscall/svc.S
    ${CMAKE_SOURCE_DIR}/src/kernel/syscall/syscall.c
    ${CMAKE_SOURCE_DIR}/src/kernel/syscall/sys_proc.c
    ${CMAKE_SOURCE_DIR}/src/kernel/syscall/sys_io.c
    ${CMAKE_SOURCE_DIR}/src/kernel/syscall/sys_time.c
    ${CMAKE_SOURCE_DIR}/src/kernel/syscall/sys_mem.c
    ${CMAKE_SOURCE_DIR}/src/kernel/syscall/sys_fs.c
    ${CMAKE_SOURCE_DIR}/src/kernel/fd/fd.c
    ${CMAKE_SOURCE_DIR}/src/kernel/fd/tty.c
    ${CMAKE_SOURCE_DIR}/src/kernel/fd/pipe.c
    ${CMAKE_SOURCE_DIR}/src/kernel/signal/signal.c
    ${CMAKE_SOURCE_DIR}/src/kernel/vfs/vfs.c
    ${CMAKE_SOURCE_DIR}/src/kernel/vfs/namei.c
    ${CMAKE_SOURCE_DIR}/src/kernel/fs/romfs.c
    ${CMAKE_SOURCE_DIR}/src/kernel/fs/devfs.c
    ${CMAKE_SOURCE_DIR}/src/kernel/fs/procfs.c
    ${CMAKE_SOURCE_DIR}/src/kernel/fs/tmpfs.c
    ${CMAKE_SOURCE_DIR}/src/kernel/fs/vfat.c
    ${CMAKE_SOURCE_DIR}/src/kernel/fs/ufs.c       # Now shared — no longer QEMU-only
    ${CMAKE_SOURCE_DIR}/src/kernel/fs/fstab.c
    ${CMAKE_SOURCE_DIR}/src/kernel/blkdev/blkdev.c
    ${CMAKE_SOURCE_DIR}/src/kernel/blkdev/loopback.c
    ${CMAKE_SOURCE_DIR}/src/kernel/exec/elf.c
    ${CMAKE_SOURCE_DIR}/src/kernel/exec/exec.c
    ${CMAKE_SOURCE_DIR}/src/kernel/smp.c
)
```

**Top-level `CMakeLists.txt` (simplified):**

```cmake
cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(ppap C CXX ASM)
pico_sdk_init()

option(PPAP_TESTS "Enable kernel integration tests and userland test suite" OFF)

include(cmake/kernel_sources.cmake)

# ── Shared pipelines (romfs, tools, user binaries) ───────────────────
# ... mkromfs, mkfatimg, mkufs ...
# User binaries: when PPAP_TESTS=ON, also build test_exec, test_vfork,
# test_pipe, test_fd, test_brk, test_signal, runtests and include them
# in romfs.  When OFF, only production binaries (hello, init, sh, ...).

# ── Targets ──────────────────────────────────────────────────────────
add_subdirectory(src/target/qemu_arm)
add_subdirectory(src/target/pico1)
add_subdirectory(src/target/pico1calc)

# ── Test support (conditional) ────────────────────────────────────────
set(PPAP_ALL_TARGETS ppap_qemu_arm ppap_pico1 ppap_pico1calc)
if(PPAP_TESTS)
    foreach(tgt ${PPAP_ALL_TARGETS})
        target_sources(${tgt} PRIVATE ${CMAKE_SOURCE_DIR}/src/test/ktest.c)
        target_compile_definitions(${tgt} PRIVATE PPAP_TESTS=1)
    endforeach()
endif()

# ── romfs + image linkage (shared by all targets) ────────────────────
foreach(tgt ${PPAP_ALL_TARGETS})
    add_dependencies(${tgt} romfs_image)
    target_sources(${tgt} PRIVATE ${CMAKE_BINARY_DIR}/romfs_data.S)
    target_include_directories(${tgt} PRIVATE src)
endforeach()

# FAT32 test image — QEMU only
add_dependencies(ppap_qemu_arm fatimg_image)
target_sources(ppap_qemu_arm PRIVATE ${CMAKE_BINARY_DIR}/fatimg_data.S)

# UF2 outputs for hardware targets
pico_add_extra_outputs(ppap_pico1)
pico_add_extra_outputs(ppap_pico1calc)
```

**`src/target/qemu_arm/CMakeLists.txt`:**

```cmake
add_executable(ppap_qemu_arm
    ${KERNEL_COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/target_qemu_arm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/uart_qemu.c
    ${CMAKE_SOURCE_DIR}/src/kernel/blkdev/ramblk.c
)
target_compile_definitions(ppap_qemu_arm PRIVATE PPAP_QEMU=1)
target_link_options(ppap_qemu_arm PRIVATE
    -T ${CMAKE_SOURCE_DIR}/ldscripts/qemu.ld
    -nostartfiles -Wl,--gc-sections
)
```

**`src/target/pico1/CMakeLists.txt`:**

```cmake
add_executable(ppap_pico1
    ${KERNEL_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/boot/stage1.S
    ${CMAKE_CURRENT_SOURCE_DIR}/target_pico1.c
    ${CMAKE_SOURCE_DIR}/src/drivers/uart.c
    ${CMAKE_SOURCE_DIR}/src/drivers/clock.c
)
target_link_libraries(ppap_pico1 pico_base_headers hardware_regs)
target_sources(ppap_pico1 PRIVATE $<TARGET_OBJECTS:bs2_default_library>)
target_link_options(ppap_pico1 PRIVATE
    -T ${CMAKE_SOURCE_DIR}/ldscripts/pico1.ld
    -nostartfiles -Wl,--gc-sections
)
```

**`src/target/pico1calc/CMakeLists.txt`:**

```cmake
add_executable(ppap_pico1calc
    ${KERNEL_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/boot/stage1.S
    ${CMAKE_CURRENT_SOURCE_DIR}/target_pico1calc.c
    ${CMAKE_SOURCE_DIR}/src/drivers/uart.c
    ${CMAKE_SOURCE_DIR}/src/drivers/clock.c
    ${CMAKE_SOURCE_DIR}/src/drivers/spi.c
    ${CMAKE_SOURCE_DIR}/src/drivers/sd.c
)
target_link_libraries(ppap_pico1calc pico_base_headers hardware_regs)
target_sources(ppap_pico1calc PRIVATE $<TARGET_OBJECTS:bs2_default_library>)
target_link_options(ppap_pico1calc PRIVATE
    -T ${CMAKE_SOURCE_DIR}/ldscripts/pico1calc.ld
    -nostartfiles -Wl,--gc-sections
)
```

**Key decisions:**
- `PPAP_QEMU=1` compile definition is **retained** for the QEMU target.
  It is no longer used in kernel code, but remains available for
  target-level code and test images.  It may be removed entirely in
  Phase 8 if no references remain.
- `PPAP_TESTS` is a top-level CMake option.  When ON, the top-level
  CMakeLists.txt adds `src/test/ktest.c` to every target's source list
  and defines `PPAP_TESTS=1`.  Userland test binaries (test_exec, …,
  runtests) are also only built and included in romfs when ON.
- The old `ppap` target is removed.  `ppap_pico1calc` replaces it.
- All three targets share `KERNEL_COMMON_SOURCES` via
  `cmake/kernel_sources.cmake`.
- Shared pipelines (romfs, tools, user binaries, image linking) remain in
  the top-level CMakeLists.txt.
- Pico SDK integration (`pico_base_headers`, `hardware_regs`,
  `bs2_default_library`) is in each RP2040 target's own CMakeLists.txt.
- Adding a new target = create `src/target/<name>/CMakeLists.txt` + add
  one `add_subdirectory()` line to the root.


### Step 7 — GDB and Flash Scripts Update

Update debug scripts for the renamed targets.

**`ppap.gdb` → `pico1calc.gdb`:**
- Change `file build/ppap.elf` → `file build/ppap_pico1calc.elf`
- Keep all OpenOCD settings (SWD, breakpoints, etc.) unchanged

**New `pico1.gdb`:**
- Same as `pico1calc.gdb` but loads `build/ppap_pico1.elf`

**`scripts/qemu.sh`:**
- Update binary path to `build/ppap_qemu_arm` (interactive shell target)
- Used for manual testing and development

**`scripts/qemu-test.sh`:**
- Reconfigures build with `-DPPAP_TESTS=ON`, builds `ppap_qemu_arm`
- Runs QEMU, greps for `ALL KERNEL TESTS PASSED` and `ALL TESTS PASSED`
- Used for CI

**New `scripts/test_all_targets.sh`:**

```bash
#!/bin/bash
set -e
cd "$(dirname "$0")/.."

echo "=== Building all targets (production) ==="
cmake -B build -DPPAP_TESTS=OFF
cmake --build build -- ppap_qemu_arm ppap_pico1 ppap_pico1calc

echo "=== Building QEMU with tests ==="
cmake -B build_test -DPPAP_TESTS=ON
cmake --build build_test -- ppap_qemu_arm

echo "=== QEMU automated test ==="
./scripts/qemu-test.sh || { echo "QEMU TEST FAIL"; exit 1; }

echo "=== Build sizes (production) ==="
arm-none-eabi-size build/ppap_qemu_arm \
                   build/ppap_pico1.elf build/ppap_pico1calc.elf

echo "=== All targets OK ==="
```


### Step 8 — Cross-Target Testing and Validation

Systematic verification that all three targets build and run correctly,
both with and without `PPAP_TESTS`.

**Build verification (PPAP_TESTS=OFF, default):**

| Check | ppap_qemu_arm | ppap_pico1 | ppap_pico1calc |
|---|---|---|---|
| `ninja` builds without error | Yes | Yes | Yes |
| No `#ifdef PPAP_QEMU` in `src/kernel/` | Yes | Yes | Yes |
| Binary size (flash) | ~500 KB (ROM) | ~400 KB (flash) | ~400 KB (flash) |
| SRAM usage (.data + .bss) | < 20 KB | < 20 KB | < 20 KB |
| UF2 output exists | N/A | Yes | Yes |
| No test code compiled | ktest.c absent | ktest.c absent | ktest.c absent |
| No test binaries in romfs | No runtests, test_* | Same | Same |

**Build verification (PPAP_TESTS=ON):**

| Check | ppap_qemu_arm | ppap_pico1 | ppap_pico1calc |
|---|---|---|---|
| `ninja` builds without error | Yes | Yes | Yes |
| ktest.c compiled + linked | Yes | Yes | Yes |
| Test binaries in romfs | runtests, test_* | Same | Same |
| Binary size increase | ~50 KB | ~50 KB | ~50 KB |

**QEMU interactive test (PPAP_TESTS=OFF):**

| Test | Expected |
|---|---|
| Boot message contains `[qemu_arm]` | "PicoPiAndPortable booting... [qemu_arm]" |
| romfs mounted at / | "VFS: romfs mounted at /" |
| fstab parse succeeds | Entries parsed, pseudo-FS mounted |
| No kernel tests run | `target_post_mount()` is no-op |
| exec /sbin/init | Boots to ash shell |
| Basic ash commands | `echo hello`, `ls /`, `cat /etc/hostname` |

**QEMU automated test (PPAP_TESTS=ON):**

| Test | Expected |
|---|---|
| Boot message contains `[qemu_arm]` | "PicoPiAndPortable booting... [qemu_arm]" |
| VFS integration tests | All PASS (open, read, stat, getdents, …) |
| Pipe integration tests | All PASS (basic, EOF, EPIPE) |
| dup/dup2 integration tests | All PASS |
| brk integration tests | All PASS (grow, write, shrink) |
| Signal integration tests | All PASS (handler, SIG_IGN) |
| Block device integration tests | All PASS (ramblk, loopback) |
| VFAT integration tests | All PASS (open, stat, mkdir, unlink) |
| Loopback integration tests | All PASS |
| tmpfs integration tests | All PASS |
| UFS integration tests | All PASS (read, write, mkdir, unlink) |
| fstab integration tests | All PASS |
| Kernel summary | "ALL KERNEL TESTS PASSED" |
| exec /bin/runtests | Userland test runner launches |
| Userland test_exec | PASS |
| Userland test_vfork | PASS |
| Userland test_pipe | PASS |
| Userland test_fd | PASS |
| Userland test_brk | PASS |
| Userland test_signal | PASS |
| Userland summary | "ALL TESTS PASSED" |

**Hardware test — pico1 (official Raspberry Pi Pico):**

| Test | Expected |
|---|---|
| Flash `ppap_pico1.uf2` via USB drag-and-drop | Board reboots |
| Boot message contains `[pico1]` | "PicoPiAndPortable booting... [pico1]" |
| System clock: 133 MHz | PLL init succeeds |
| No SD messages | SD init skipped (no `TARGET_CAP_SD`) |
| romfs mounted, fstab: devfs/procfs/tmpfs | Pseudo-FS mounts succeed |
| VFAT/loopback entries skipped | fstab gracefully skips (no block device) |
| ash shell prompt | `ppap# ` appears on UART |
| `ls /`, `echo hello`, `uname -a` | Commands execute correctly |
| `cat /proc/meminfo` | Shows free pages |

**Hardware test — pico1calc (ClockworkPi PicoCalc):**

| Test | Expected |
|---|---|
| Flash `ppap_pico1calc.uf2` via SWD | Board reboots |
| Boot message contains `[pico1calc]` | "PicoPiAndPortable booting... [pico1calc]" |
| SPI0 init + SD card detected | SD init succeeds, mmcblk0 registered |
| VFAT mounted at /mnt/sd | fstab mount succeeds |
| UFS loopback mounts at /usr, /home, /var | If images present on SD |
| ash shell with full FS access | `ls /mnt/sd`, file I/O on SD |

**Regression checks:**
- All Phase 6 exit criteria still pass on all three targets
- `PPAP_TESTS=ON` build passes both kernel and userland test suites on QEMU
- `PPAP_TESTS=OFF` (default) builds boot to interactive shell (no test output)
- No increase in kernel SRAM usage (target_*.c data is in .text on flash)
- Boot time unchanged (target_early_init/late_init is a reorganization, not
  new work)

---

## Deliverables

| File | Description |
|---|---|
| `src/target/target.h` | Target abstraction API: early_init, late_init, post_mount, init_path, caps |
| `src/target/qemu_arm/CMakeLists.txt` | ppap_qemu_arm target definition |
| `src/target/qemu_arm/target_qemu_arm.c` | QEMU target implementation (test-aware via PPAP_TESTS) |
| `src/target/qemu_arm/drivers/uart_qemu.c` | CMSDK UART driver (moved from src/drivers/) |
| `tests/kernel/ktest.c` | Kernel integration tests (extracted from main_qemu.c, compiled only with PPAP_TESTS=ON) |
| `tests/kernel/ktest.h` | ktest_run_all() declaration |
| `src/target/pico1/CMakeLists.txt` | ppap_pico1 target definition |
| `src/target/pico1/target_pico1.c` | Official Pico target implementation |
| `src/target/pico1/pico1.h` | Pico GPIO pin definitions |
| `src/target/pico1calc/CMakeLists.txt` | ppap_pico1calc target definition |
| `src/target/pico1calc/target_pico1calc.c` | PicoCalc target implementation |
| `src/target/pico1calc/pico1calc.h` | PicoCalc GPIO pin definitions (merged + renamed) |
| `cmake/kernel_sources.cmake` | Shared KERNEL_COMMON_SOURCES list |
| `src/kernel/main.c` | Unified kernel entry point (target-agnostic) |
| `src/kernel/fs/fstab.c` | UFS support unconditional (no ifdef) |
| `ldscripts/pico1.ld` | Linker script for official Pico (2 MB flash) |
| `ldscripts/pico1calc.ld` | Linker script for PicoCalc (renamed from ppap.ld) |
| `CMakeLists.txt` | Project setup, shared pipelines, add_subdirectory per target |
| `scripts/test_all_targets.sh` | Build + smoke test all 3 targets (production + test) |
| `pico1.gdb` | GDB script for official Pico debugging |
| `pico1calc.gdb` | GDB script for PicoCalc (renamed from ppap.gdb) |

**Deleted files:**

| File | Reason |
|---|---|
| `src/kernel/main_qemu.c` | Init logic → `target_qemu_arm.c`; tests → `tests/kernel/ktest.c` |
| `src/board/picocalc.h` | Merged + renamed to `src/target/pico1calc/pico1calc.h` (old `src/board/` removed) |
| `src/drivers/uart_qemu.c` | Moved to `src/target/qemu_arm/drivers/uart_qemu.c` |
| `ldscripts/ppap.ld` | Renamed to `ldscripts/pico1calc.ld` |
| `ppap.gdb` | Renamed to `pico1calc.gdb` |

---

## Known Risks and Mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| pico1 2 MB flash too small for busybox romfs | Low | Minimal busybox (~300 KB) + config (~2 KB) fits in 1.9 MB; reduce applets if needed |
| UFS code on hardware targets wastes ~8 KB flash | Low | 8 KB is within the 64 KB kernel region budget; gc-sections eliminates unused code if UFS is never called |
| Target abstraction adds function call overhead | Negligible | target_early_init/late_init/post_mount called once at boot; no hot-path impact |
| Renaming ppap → ppap_pico1calc breaks existing scripts/habits | Medium | Update all scripts, GDB files, and CI in the same step; old `ppap` target is removed cleanly |
| PPAP_TESTS=ON bloats production romfs | Low | Test binaries only built and included when PPAP_TESTS=ON; default OFF produces a clean romfs |
| ktest.c extraction breaks test functions | Low | Tests are self-contained; extraction is mechanical (move functions, add ktest.h header) |
| smp.c Core 1 code linked into QEMU target but unused | Low | `core1_launch()` is called from target_late_init only on HW targets; gc-sections may remove it from QEMU binary |
| Official Pico GPIO conflicts (UART0 on GP0/GP1) | Low | GP0/GP1 for UART0 is the Pico default; no conflicts with other peripherals on the official Pico |
| Removing #ifdef PPAP_QEMU from fstab.c causes UFS link errors on HW | Low | Add ufs.c to all targets' source lists; verify with a clean build |

---

## References

- [PicoPiAndPortable Design Spec v0.4](PicoPiAndPortable-spec-v04.md) — §1.4 (Build Targets), §9 (Development Roadmap, Phase 7)
- [Raspberry Pi Pico Datasheet](https://datasheets.raspberrypi.com/pico/pico-datasheet.pdf) — Pico GPIO pinout, 2 MB flash capacity
- [RP2040 Datasheet](https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf) — UART, SPI, GPIO function select
- [ClockworkPi PicoCalc](https://www.clockworkpi.com/picocalc) — PicoCalc schematic, SPI0 pin assignments
- [CMake add_executable](https://cmake.org/cmake/help/latest/command/add_executable.html) — Shared source variables
- [QEMU mps2-an500](https://www.qemu.org/docs/master/system/arm/mps2.html) — Emulated board reference
