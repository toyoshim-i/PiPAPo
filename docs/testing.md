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

# Everything at once (host + build all targets + QEMU tests)
./scripts/test.sh --all
```

## Host unit tests

Pure C unit tests compiled with the system gcc/clang. No cross-compiler
toolchain needed. Good for testing kernel modules that have no hardware
dependencies (memory allocators, ELF parser, fd bookkeeping).

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
| `test_fd.c` | File descriptor table operations |
| `test_elf.c` | ELF32 header/segment parser |
| `test_ecpu_z80.c` | Z80 emulator (85 tests: all instruction groups) |
| `test_cpm_bridge.c` | CP/M BDOS bridge (42 tests: console, file, disk, integration) |
| `test_ecpu_m68k.c` | m68k emulator (instruction groups, addressing modes) |

### Build system

`tests/host/CMakeLists.txt` defines one executable per module. Stubs in
`tests/host/stubs/` provide UART, TTY, and XIP no-ops so kernel sources
link on the host.

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
   target_link_libraries(test_foo PRIVATE stubs)   # if needed
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
5. After kernel tests, `target_init_path()` returns `/bin/runtests`
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
Prints `RUN`, then `PASS` or `FAIL` per test. Final summary: counts +
`ALL TESTS PASSED` or `SOME TESTS FAILED`.

The test list is initialised at runtime (not as a static array) because
PIC binaries cannot have initialised pointer arrays — the ELF loader
only relocates GOT entries, not arbitrary data pointers.

### Files

| File | Tests |
|------|-------|
| `test_exec.c` | ELF loading, XIP, GOT relocation |
| `test_vfork.c` | `vfork` + `execve` + `waitpid` |
| `test_pipe.c` | Pipe creation, read/write, EOF |
| `test_brk.c` | Heap growth via `brk()` |
| `test_fd.c` | `dup`, `dup2`, `close`, redirection |
| `test_signal.c` | Signal handler install + delivery |
| `test_poll.c` | `ppoll` syscall |
| `test_sleep_intr.c` | Process lifecycle, exit codes |
| `test_orphan.c` | Orphan reparenting to init |
| `test_fault.c` | CPU fault handlers (illegal insn, div-by-zero) |
| `test_id.c` | `getpid`, `getppid`, `getuid`, `getgid` |
| `test_fs.c` | Filesystem operations (open, read, readdir, stat) |
| `test_rw.c` | File read/write on writable filesystems |
| `test_time.c` | `clock_gettime`, `gettimeofday` |
| `test_iov.c` | `readv`, `writev` scatter/gather I/O |
| `test_stat.c` | `stat64`, `fstat64`, `lstat64` |
| `test_tmpfs.c` | tmpfs create, write, read, unlink |
| `test_x68k.c` | Human68k subsystem (R-format execution) |
| `test_cpm.c` | CP/M subsystem (13 tests: BDOS calls, .COM exec, warm boot) |

### Build system

User tests are built by `cmake/user.cmake` as custom commands, controlled
by the `PPAP_TESTS` CMake option. The build system:

- Cross-compiles each `test_*.c` from `tests/user/`
- Builds Human68k R-format user tests from `tests/user/r68k/` on m68k targets
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
3. Add `"/bin/test_foo"` to the test list in `tests/user/runtests.c`
4. Build with `--test` and run

### Constraints for user-space test code

- **No libc.** Only `syscall.h` wrappers are available.
- **No division on m68k.** GCC emits `__divsi3` calls; use
  subtraction loops (see `ut_print_int` in `utest.h`).
- **No static pointer arrays.** PIC relocation only fixes GOT entries.
  Initialise pointer arrays at runtime, or use a `switch` statement
  returning string literals (which are GOT-resolved per call site).
- **Use `vfork` + `execve`, not `fork`.** PPAP has no MMU; `vfork`
  shares the parent's address space. The child must immediately
  `execve` or `_exit` — do not modify parent data or trigger faults.

## Automated QEMU testing

### `run.sh --test`

Builds with `PPAP_TESTS=ON`, runs under QEMU with a 30-second timeout,
and greps output for `ALL.*TESTS PASSED`.

```bash
./scripts/run.sh --test              # ARM (default)
./scripts/run.sh --test qemu_m68k    # m68k
```

### `test.sh --all`

Full CI pipeline:
1. Host unit tests
2. Build all production targets (ARM and m68k, `PPAP_TESTS=OFF`)
3. Print binary sizes
4. QEMU on-target tests (ARM and m68k, `PPAP_TESTS=ON`)

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
                            ├── test_exec
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
                            ├── test_x68k
                            ├── test_cpm
                            └── test_h68k_dos (m68k only)
                                   │
                        "ALL TESTS PASSED"
                                   │
                   run.sh --test checks exit
```

## Build flags reference

| Flag | Where | Effect |
|------|-------|--------|
| `--test` | `./scripts/build.sh` | Sets `PPAP_TESTS=ON` in CMake |
| `PPAP_TESTS=ON` | CMake option | Compiles `ktest.c` into kernel; defines `PPAP_TESTS=1` C macro; enables user test builds |
| `PPAP_TESTS=1` | C preprocessor | Guards `ktest_run_all()` call in `target_post_mount()`; selects `/bin/runtests` as init path |
| `PPAP_TESTS=ON` | `cmake/user.cmake` | Builds test binaries and installs to romfs |
