# Phase 6: musl + busybox — Detailed Plan

**PicoPiAndPortable Development Roadmap**
Estimated Duration: 4 weeks
Prerequisites: Phase 5 (UFS + Loopback) complete
**Status: Complete**

---

## Goals

Port musl libc to PicoPiAndPortable's syscall interface, cross-compile busybox
for ARMv6-M Thumb, fill in syscall gaps, and bring up an interactive ash shell.
After this phase the system boots to a working shell prompt where users can run
basic commands (ls, cat, echo, grep, etc.) over the UART console.

**Exit Criteria (all must pass before moving to Phase 7):**
- musl libc cross-compiled for armv6m-thumb with PPAP syscall wrappers
- busybox statically linked with ported musl in minimal config (ash + coreutils)
- busybox binary (200–400 KB) placed in romfs at `/bin/busybox` and executed via XIP
- Symlink farm in romfs (`/bin/ls` → `/bin/busybox`, etc.) resolved by VFS
- `/sbin/init` (busybox init) launched as PID 1 from `kmain()`
- init reads `/etc/inittab` and spawns ash on `/dev/ttyS0`
- ash shell displays a prompt (`$ `) and accepts typed commands
- Basic command execution works: `ls /`, `cat /etc/hostname`, `echo hello`
- Pipelines work: `echo hello | cat`, `ls / | grep etc`
- Signal handling works: Ctrl-C sends SIGINT to foreground process
- Job control basics: `kill`, `ps` applets function
- All 24 existing syscalls + new syscalls (≥ 40 total) pass musl/busybox smoke tests
- QEMU smoke test: busybox ash launches and runs simple commands
- Hardware test: interactive ash session over UART on PicoCalc

---

## Third-Party Source Management

musl and busybox are imported as **git submodules** under `third_party/`.
Upstream sources are pinned to a specific release tag and never modified
directly.  PPAP-specific patches are maintained as patch files in
`third_party/patches/` and applied at build time via a helper script.

**Directory layout:**

```
third_party/
  musl/                       # git submodule → git://git.musl-libc.org/musl (tag v1.2.5)
  busybox/                    # git submodule → https://git.busybox.net/busybox (tag 1_36_1)
  patches/
    musl/
      0001-arm-syscall_arch-ppap-svc-wrappers.patch
      0002-arm-atomic_arch-cortex-m0-compat.patch
      0003-thread-lock-single-thread-noop.patch
    busybox/
      0001-configs-ppap-nommu-defconfig.patch
  configs/
    busybox_ppap_defconfig    # Saved busybox .config for PPAP minimal build
  build-musl.sh               # Configure + patch + build musl libc.a
  build-busybox.sh            # Configure + patch + build busybox binary
```

**Submodule setup:**

```sh
git submodule add --depth 1 -b v1.2.5 \
    git://git.musl-libc.org/musl third_party/musl
git submodule add --depth 1 -b 1_36_1 \
    https://git.busybox.net/busybox third_party/busybox
```

**Patch workflow:**

Patches are generated with `git diff` against the submodule's clean state
and applied at build time.  This keeps the submodule itself unmodified
(clean `git status`) while maintaining reproducible builds.

```sh
# third_party/build-musl.sh (simplified)
#!/bin/bash
set -e
MUSL_SRC=third_party/musl
MUSL_BUILD=build/musl
MUSL_PREFIX=build/musl-sysroot

# Start from clean submodule state
cd "$MUSL_SRC"
git checkout .
for p in ../patches/musl/*.patch; do
    git apply "$p"
done

./configure \
    --target=arm-none-eabi \
    --prefix="$(realpath ../../$MUSL_PREFIX)" \
    --disable-shared --enable-static \
    CROSS_COMPILE=arm-none-eabi- \
    CC=arm-none-eabi-gcc \
    CFLAGS="-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft -Os"
make -j$(nproc)
make install
cd ../..

# Restore submodule to clean state after build
cd "$MUSL_SRC" && git checkout .
```

**Why submodules + patches (not fork or vendor):**
- **Reproducibility:** submodule pins exact upstream commit; patches are
  explicit and reviewable
- **Upgradeability:** to update musl or busybox, bump the submodule tag
  and rebase patches — minimal maintenance burden
- **Clean `git status`:** the submodule stays pristine; patches are applied
  only during build (and reverted after)
- **Small repo:** no need to check in ~50 MB of third-party source

**Build order (CMakeLists.txt):**
1. `build-musl.sh` → produces `build/musl-sysroot/lib/libc.a` + headers
2. `build-busybox.sh` → produces `build/busybox/busybox` (static ELF)
3. Install busybox + symlinks into `romfs/bin/`
4. `mkromfs` → produces `build/romfs.bin`
5. Link kernel + romfs into final ELF

---

## Source Tree After Phase 6

```
third_party/
  musl/                       # git submodule (v1.2.5) — upstream unmodified
  busybox/                    # git submodule (1_36_1) — upstream unmodified
  patches/
    musl/
      0001-arm-syscall_arch-ppap-svc-wrappers.patch
      0002-arm-atomic_arch-cortex-m0-compat.patch
      0003-thread-lock-single-thread-noop.patch
    busybox/
      0001-configs-ppap-nommu-defconfig.patch
  configs/
    busybox_ppap_defconfig    # Saved .config for PPAP minimal build
  build-musl.sh               # Patch + configure + build musl libc.a
  build-busybox.sh            # Patch + configure + build busybox binary
src/
  kernel/
    main.c                    (existing — add init process launch)
    main_qemu.c               (existing — same init launch)
    syscall/
      syscall.c               (existing — extend dispatch table to ~50 entries)
      syscall.h               (existing — add new syscall numbers)
      svc.S                   (existing — handle 6-arg syscalls via r5/r6)
      sys_proc.c              (existing — add getuid/gid, setpgid, setsid, uname)
      sys_io.c                (existing — add writev, ioctl, fcntl)
      sys_fs.c                (existing — add access, readlink, umask, ftruncate,
                                          link, symlink, rename, lstat, mknod)
      sys_mem.c               (existing — add mmap/munmap anonymous)
      sys_time.c              (existing — add clock_gettime, gettimeofday, times)
      sys_signal.c            # New: sigprocmask, sigsuspend, sigpending
    fd/
      tty.c/h                 (existing — add termios support, line discipline)
    fs/
      romfs.c                 (existing)
      devfs.c                 (existing — add /dev/tty, /dev/console)
      procfs.c                (existing — add /proc/self, /proc/<pid>/*)
    exec/
      exec.c                  (existing — add argv/envp passing, large binary support)
    signal/
      signal.c/h              (existing — add sigprocmask, blocked mask)
  drivers/
    uart.c/h                  (existing)
user/
  (existing test binaries remain)
  test_musl.c                 # New: musl-linked "hello world" using printf
  test_ash.c                  # New: fork+exec /bin/ash, feed commands via pipe
tools/
  mkromfs/                    (existing)
romfs/
  bin/
    busybox                   # Static ELF: musl-linked busybox (~200–400 KB)
    sh -> busybox             # Symlinks for multicall
    ash -> busybox
    ls -> busybox
    cat -> busybox
    echo -> busybox
    printf -> busybox
    grep -> busybox
    head -> busybox
    tail -> busybox
    wc -> busybox
    sort -> busybox
    cp -> busybox
    mv -> busybox
    rm -> busybox
    mkdir -> busybox
    rmdir -> busybox
    ln -> busybox
    chmod -> busybox
    ps -> busybox
    kill -> busybox
    sleep -> busybox
    mount -> busybox
    umount -> busybox
    df -> busybox
    free -> busybox
    uname -> busybox
    dmesg -> busybox
    sed -> busybox
    init -> busybox            # PID 1 (multicall as /sbin/init or /bin/init)
  sbin/
    init -> ../bin/busybox     # Alternate path for init
  etc/
    hostname                  (existing — "ppap")
    motd                      (existing)
    inittab                   # New: busybox init configuration
    fstab                     # New: filesystem mount table
    passwd                    # New: root:x:0:0:root:/root:/bin/ash
    group                     # New: root:x:0:
    profile                   # New: shell startup (PATH, PS1, motd)
  root/                       # Home directory for root user (empty)
```

---

## Week 1: musl libc Porting

### Step 1 — musl Source Integration and Cross-Compile Setup

Obtain musl libc source and configure it for cross-compilation targeting
ARMv6-M Thumb (Cortex-M0+).  musl is chosen over newlib because its syscall
layer is a thin wrapper around Linux system calls — easy to redirect to
PPAP's SVC interface.

**musl version:** 1.2.5 (latest stable).

**Integration approach:** musl is added as a git submodule under
`third_party/musl/` (see "Third-Party Source Management" above).
Three patch files modify the syscall arch layer, atomics, and locking.
The `third_party/build-musl.sh` script applies patches, runs configure,
builds `libc.a`, and installs headers into `build/musl-sysroot/`.

**Patch files created in this step:**

| Patch | Target File | Change |
|---|---|---|
| `0001-arm-syscall_arch-ppap-svc-wrappers.patch` | `arch/arm/syscall_arch.h` | Replace Linux syscall inline asm with PPAP SVC (Step 2) |
| `0002-arm-atomic_arch-cortex-m0-compat.patch` | `arch/arm/atomic_arch.h` | Replace LDREX/STREX with M0+-safe CAS (Step 3) |
| `0003-thread-lock-single-thread-noop.patch` | `src/thread/__lock.c` | No-op locking for single-threaded processes (Step 3) |

**Build script (`third_party/build-musl.sh`) key steps:**

```sh
# 1. Apply patches to submodule working tree
# 2. Run musl's configure:
./configure \
  --target=arm-none-eabi \
  --prefix="$MUSL_SYSROOT" \
  --disable-shared --enable-static \
  CROSS_COMPILE=arm-none-eabi- \
  CC=arm-none-eabi-gcc \
  CFLAGS="-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft -Os"
# 3. make -j$(nproc) && make install
# 4. Restore submodule to clean state (git checkout .)
```

This produces `build/musl-sysroot/lib/libc.a` and headers under
`build/musl-sysroot/include/`.

**Build integration with busybox:** busybox's build system is pointed at
the installed musl sysroot via `CONFIG_SYSROOT` and `CONFIG_EXTRA_CFLAGS`.

**Gotcha — ARMv6-M Thumb restrictions:**
- No `__sync_*` or `__atomic_*` builtins on Cortex-M0+ (no LDREX/STREX).
  musl's `atomic_arch.h` for ARM uses these.  We must provide Cortex-M0+
  compatible atomics using interrupt disable/enable (acceptable for single-core
  user-mode on PPAP where only Core 0 runs user code).
- No hardware divide instruction — musl/compiler-rt provides `__aeabi_idiv`
  and `__aeabi_uidiv` in software.  Ensure these are linked.

**Verification:** compile a minimal `hello.c` using musl's `printf`:

```c
#include <stdio.h>
int main(void) {
    printf("Hello from musl!\n");
    return 0;
}
```

Link statically with `libc.a`, verify the resulting ELF references `SVC 0`
for `write` and `exit_group` (or the PPAP equivalents after Step 2).


### Step 2 — Syscall Arch Header Replacement

Replace musl's `arch/arm/syscall_arch.h` to redirect all system calls
through PPAP's SVC interface.  This is the single most critical porting step.

**Original musl `syscall_arch.h` (Linux ARM):**

```c
static inline long __syscall0(long n) {
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0");
    __asm__ volatile("svc 0" : "=r"(r0) : "r"(r7) : "memory");
    return r0;
}
/* … __syscall1 through __syscall6 */
```

**PPAP replacement (`musl/arch/arm/syscall_arch.h`):**

```c
/* PicoPiAndPortable — musl syscall arch for ARMv6-M
 * Uses the same ARM EABI convention as Linux:
 *   r7 = syscall number
 *   r0–r3 = arguments (4-arg syscalls)
 *   r4, r5 = arguments 5–6 (rare; e.g., mmap)
 *   r0 = return value (negative errno on error)
 *   svc 0 = trap to kernel
 *
 * PPAP uses Linux-compatible syscall numbers for the subset it implements.
 * Unimplemented syscalls return -ENOSYS.
 */

#define __SYSCALL_LL_E(x) \
  ((union { long long ll; long l[2]; }){ .ll = x }).l[0], \
  ((union { long long ll; long l[2]; }){ .ll = x }).l[1]
#define __SYSCALL_LL_O(x) 0, __SYSCALL_LL_E(x)

static inline long __syscall0(long n) {
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0");
    __asm__ __volatile__("svc 0"
        : "=r"(r0) : "r"(r7) : "memory");
    return r0;
}

static inline long __syscall1(long n, long a) {
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    __asm__ __volatile__("svc 0"
        : "=r"(r0) : "r"(r7), "0"(r0) : "memory");
    return r0;
}

static inline long __syscall2(long n, long a, long b) {
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    __asm__ __volatile__("svc 0"
        : "=r"(r0) : "r"(r7), "0"(r0), "r"(r1) : "memory");
    return r0;
}

static inline long __syscall3(long n, long a, long b, long c) {
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    __asm__ __volatile__("svc 0"
        : "=r"(r0) : "r"(r7), "0"(r0), "r"(r1), "r"(r2) : "memory");
    return r0;
}

static inline long __syscall4(long n, long a, long b, long c, long d) {
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    __asm__ __volatile__("svc 0"
        : "=r"(r0) : "r"(r7), "0"(r0), "r"(r1), "r"(r2), "r"(r3)
        : "memory");
    return r0;
}

/* 5- and 6-arg syscalls pass args in r4, r5 (saved/restored by caller).
 * Only mmap uses 6 args; most syscalls use ≤ 4. */
static inline long __syscall5(long n, long a, long b, long c, long d,
                               long e) {
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r4 __asm__("r4") = e;
    __asm__ __volatile__("svc 0"
        : "=r"(r0) : "r"(r7), "0"(r0), "r"(r1), "r"(r2), "r"(r3),
          "r"(r4) : "memory");
    return r0;
}

static inline long __syscall6(long n, long a, long b, long c, long d,
                               long e, long f) {
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r4 __asm__("r4") = e;
    register long r5 __asm__("r5") = f;
    __asm__ __volatile__("svc 0"
        : "=r"(r0) : "r"(r7), "0"(r0), "r"(r1), "r"(r2), "r"(r3),
          "r"(r4), "r"(r5) : "memory");
    return r0;
}
```

**Syscall number mapping:** PPAP already uses Linux ARM EABI syscall numbers
(e.g., SYS_exit=1, SYS_read=3, SYS_write=4, SYS_open=5).  musl's internal
`__NR_*` constants match.  For unimplemented syscalls, the kernel returns
`-ENOSYS` — musl handles this gracefully in most paths.

**SVC handler update (`svc.S`):** The current SVC handler extracts r0–r3
from the hardware exception frame but does not handle r4/r5 for 5–6 arg
syscalls.  Update the handler to also read r4/r5 from the caller's saved
context (these are callee-saved registers, so they are in the PendSV software
frame or still live in the actual registers at SVC entry):

```asm
/* In SVC_Handler, after extracting r0-r3 from exception frame: */
    /* r4, r5 are callee-saved — still live in registers at SVC entry */
    mov  r4, r4          /* r4 = 5th arg (already in register) */
    mov  r5, r5          /* r5 = 6th arg (already in register) */
```

Since r4 and r5 are callee-saved by the C ABI, the `__syscall5/6` inline
asm loads them before `svc 0` and the kernel reads them directly — no
special frame extraction needed.  The dispatch function signature changes
to accept up to 6 arguments.


### Step 3 — Thread and TLS Stubs

musl's internal locking and TLS (Thread-Local Storage) mechanisms assume
a pthreads-capable environment.  Since PPAP is single-threaded per process
(no pthreads), these are stubbed out.

**Locking stubs (`musl/src/thread/__lock.c`):**

musl uses `__lock` and `__unlock` for internal state (malloc, stdio buffers).
On a single-threaded system, these become no-ops:

```c
void __lock(volatile int *l) { (void)l; }
void __unlock(volatile int *l) { (void)l; }
```

**TLS (`__aeabi_read_tp`):**

musl's ARM port calls `__aeabi_read_tp()` to get the thread pointer.  On
Linux this is a kernel-assisted TLS mechanism.  On PPAP, with a single thread
per process, we provide a simple static TLS area:

```c
/* Provide a per-process TLS block.  Since PPAP processes are single-threaded,
 * a fixed static area suffices.  The TLS block is allocated at the top of
 * the process's data page during exec(). */
static char tls_block[64] __attribute__((aligned(8)));

void *__aeabi_read_tp(void) {
    return tls_block;
}
```

This function is compiled into `libc.a` and linked into every busybox
binary.  The 64-byte TLS block is sufficient for musl's `struct pthread`
stub and errno.

**`errno` location:** musl stores errno in the thread structure accessed
via `__aeabi_read_tp`.  With the static TLS block, errno works correctly
for single-threaded processes.

**pthreads stubs:** musl's `pthread_create` and friends are only linked
if busybox actually calls them (it does not in the minimal config).
If needed, `pthread_create` returns `ENOSYS`.

**Atomic operations (`musl/arch/arm/atomic_arch.h`):**

The default ARM atomics use LDREX/STREX, which are not available on
Cortex-M0+.  Replace with interrupt-disable-based atomics:

```c
/* Cortex-M0+ has no LDREX/STREX.  Since PPAP user code runs single-threaded
 * on Core 0, we can use interrupt-disable for atomics.  This works because
 * musl only uses atomics for internal locking, which is no-op anyway. */

static inline int a_cas(volatile int *p, int t, int s) {
    /* CAS is only needed for locking — with no-op locks, this is trivial */
    int old = *p;
    if (old == t) *p = s;
    return old;
}

#define a_barrier() __asm__ __volatile__("" ::: "memory")
```

**Verification:** compile musl's `libc.a` with these stubs.  Link the
musl hello test from Step 1.  Run on QEMU — `printf("Hello from musl!\n")`
should produce output via `SVC write`.

---

## Week 2: busybox Build and romfs Integration

### Step 4 — busybox Minimal Configuration

Cross-compile busybox for ARMv6-M Thumb using the ported musl libc.
The goal is the smallest functional configuration that provides ash and
essential coreutils.

**busybox version:** 1.36.1 (pinned via git submodule tag `1_36_1`).

busybox lives at `third_party/busybox/`.  A saved defconfig is stored at
`third_party/configs/busybox_ppap_defconfig` and copied into the busybox
tree at build time by `third_party/build-busybox.sh`.  The build script
points busybox at the musl sysroot from Step 1:

```sh
# third_party/build-busybox.sh (simplified)
cd third_party/busybox
cp ../configs/busybox_ppap_defconfig .config
make ARCH=arm CROSS_COMPILE=arm-none-eabi- \
     CFLAGS_EXTRA="--sysroot=$MUSL_SYSROOT -mthumb -mcpu=cortex-m0plus" \
     -j$(nproc)
```

**Configuration approach:** start from `make allnoconfig`, then enable
the minimum applet set from spec §6.2:

```
# .config excerpt — busybox minimal for PPAP

# General
CONFIG_STATIC=y
CONFIG_CROSS_COMPILER_PREFIX="arm-none-eabi-"
CONFIG_SYSROOT="/opt/ppap-musl"
CONFIG_EXTRA_CFLAGS="-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft -Os"
CONFIG_EXTRA_LDFLAGS="-static"

# Shell
CONFIG_ASH=y
CONFIG_ASH_JOB_CONTROL=y
CONFIG_ASH_ALIAS=y
CONFIG_ASH_BUILTIN_ECHO=y
CONFIG_ASH_BUILTIN_PRINTF=y
CONFIG_ASH_BUILTIN_TEST=y
CONFIG_ASH_CMDCMD=y
CONFIG_ASH_OPTIMIZE_FOR_SIZE=y
# Disable features that need Linux-specific syscalls
CONFIG_ASH_MAIL=n
CONFIG_ASH_RANDOM_SUPPORT=n

# File Operations
CONFIG_LS=y
CONFIG_CP=y
CONFIG_MV=y
CONFIG_RM=y
CONFIG_CAT=y
CONFIG_MKDIR=y
CONFIG_RMDIR=y
CONFIG_LN=y
CONFIG_CHMOD=y

# Text Processing
CONFIG_ECHO=y
CONFIG_PRINTF=y
CONFIG_GREP=y
CONFIG_HEAD=y
CONFIG_TAIL=y
CONFIG_WC=y
CONFIG_SORT=y
CONFIG_SED=y

# Process Management
CONFIG_PS=y
CONFIG_KILL=y
CONFIG_SLEEP=y

# System
CONFIG_MOUNT=y
CONFIG_UMOUNT=y
CONFIG_DF=y
CONFIG_FREE=y
CONFIG_UNAME=y
CONFIG_DMESG=y

# Init
CONFIG_INIT=y
CONFIG_FEATURE_INIT_SCTTY=y
CONFIG_FEATURE_INIT_QUIET=n

# Disable Linux-specific features
CONFIG_FEATURE_USE_SENDFILE=n
CONFIG_FEATURE_UTMP=n
CONFIG_FEATURE_WTMP=n
CONFIG_MODPROBE_SMALL=n
CONFIG_FEATURE_SYSLOG=n
```

**Build:**

```sh
cd busybox
make ARCH=arm CROSS_COMPILE=arm-none-eabi- defconfig  # or use saved .config
# Apply minimal .config overrides
make ARCH=arm CROSS_COMPILE=arm-none-eabi- -j$(nproc)
```

**Expected binary size:** 200–350 KB for this applet set with `-Os` and
musl static linking.  This fits comfortably in the 16 MB romfs flash region.

**Gotcha — `fork()` vs `vfork()`:** busybox uses `fork()` in many places.
musl's `fork()` calls the `clone` syscall on Linux.  PPAP provides `vfork()`
only (spec §4.3).  Options:
1. Map musl's `fork()` → PPAP's `vfork()`.  This works for the common
   busybox pattern of `fork() + execve()` but is unsafe if the child
   modifies the parent's stack (vfork shares the address space).
2. Patch busybox to use `vfork()` explicitly (busybox has `CONFIG_VFORK`
   support — enable it).

**Recommended:** enable `CONFIG_NOMMU=y` in busybox, which switches all
fork paths to `vfork()`.  This is the standard approach for MMU-less systems
(uClinux, NUTTX, FUZIX).

**Additional busybox NOMMU settings:**

```
CONFIG_NOMMU=y
CONFIG_FEATURE_SH_STANDALONE=y    # busybox runs applets directly (no fork+exec)
CONFIG_FEATURE_PREFER_APPLETS=y   # use internal applets instead of /bin/xxx
```

`CONFIG_FEATURE_SH_STANDALONE` is critical for performance on NOMMU: instead
of `vfork() + execve("/bin/ls")`, ash calls the `ls` applet function directly
within the same process.  This avoids the vfork overhead for most commands.

**Verification:** the resulting `busybox` ELF should be `ET_EXEC` or `ET_DYN`
for ARM, statically linked (no `PT_INTERP`), and contain all enabled applets.
Confirm with:

```sh
arm-none-eabi-readelf -h busybox         # Machine: ARM, Class: ELF32
arm-none-eabi-readelf -l busybox         # PT_LOAD segments only
arm-none-eabi-size busybox               # text + data + bss totals
./busybox --list                          # (on QEMU or cross-run) lists applets
```


### Step 5 — Syscall Gap Analysis

Before romfs integration, systematically identify which syscalls busybox
ash actually invokes at runtime.  This prevents the frustrating cycle of
"ash starts, hits ENOSYS, crashes".

**Method 1 — Static analysis:**

Use `arm-none-eabi-objdump -d busybox | grep "svc"` to find all SVC call
sites.  Cross-reference `r7` values loaded before each `svc 0` to build
the complete syscall number list.

**Method 2 — Runtime tracing on QEMU:**

Add a debug mode to the kernel's `syscall_dispatch()` that prints every
syscall number and its return value.  Boot busybox ash on QEMU and observe
the sequence.

**Expected syscall requirements beyond current 24:**

| Category | Syscall | Number | Why needed |
|---|---|---|---|
| Process | `getuid` / `geteuid` | 199/201 | ash prompt, `id` command |
| Process | `getgid` / `getegid` | 200/202 | same |
| Process | `getppid` | 64 | process management |
| Process | `setpgid` / `getpgid` | 57/132 | job control |
| Process | `setsid` | 66 | init creates session |
| Process | `uname` | 122 | `uname` command |
| Process | `exit_group` | 248 | musl's `_exit` uses this |
| File | `readlink` / `readlinkat` | 85/263 | `/proc/self/exe`, symlinks |
| File | `access` / `faccessat` | 33/334 | `test -e`, `test -r` |
| File | `fcntl` / `fcntl64` | 55/221 | `F_GETFD`, `F_SETFD`, `O_CLOEXEC` |
| File | `ftruncate` | 93 | file creation |
| File | `umask` | 60 | file creation mode |
| File | `link` | 9 | `ln` command |
| File | `symlink` | 83 | `ln -s` command |
| File | `rename` | 38 | `mv` command |
| File | `lstat` | 107 | `ls -l` (distinguish symlinks) |
| File | `mknod` | 14 | devfs (stub) |
| File | `openat` | 322 | musl uses `*at` variants internally |
| I/O | `writev` | 146 | musl stdio buffered output |
| I/O | `ioctl` | 54 | terminal control (TIOCGWINSZ, TCGETS) |
| Memory | `mmap2` | 192 | musl malloc large allocations |
| Memory | `munmap` | 91 | musl malloc free |
| Signal | `rt_sigprocmask` | 175 | ash signal handling |
| Signal | `rt_sigaction` | 174 | musl wraps sigaction |
| Signal | `sigreturn` | 119 | signal return (already have) |
| Time | `clock_gettime` | 263 | musl `time()`, `gettimeofday()` |
| Time | `gettimeofday` | 78 | timestamps |
| Time | `times` | 43 | `time` builtin |
| System | `set_tid_address` | 256 | musl startup (stub: return getpid) |
| System | `set_tls` | 0xF0005 | ARM `__aeabi_read_tp` (via kuser helper or SVC) |

**Priority classification:**
- **P0 (must have for boot):** `exit_group`, `rt_sigprocmask`, `writev`,
  `fcntl`, `ioctl`, `mmap2`, `munmap`, `set_tid_address`, `getuid`,
  `setpgid`, `setsid`, `uname`, `clock_gettime`
- **P1 (must have for ash):** `access`, `readlink`, `umask`, `lstat`,
  `getppid`, `getpgid`, `gettimeofday`, `openat`
- **P2 (nice to have for coreutils):** `link`, `symlink`, `rename`,
  `ftruncate`, `mknod`, `times`, `getgid`, `geteuid`, `getegid`

Steps 7–9 implement these in priority order.


### Step 6 — romfs Integration with busybox

Integrate the busybox binary and symlink farm into the romfs image.
Update mkromfs and the build pipeline to handle the larger binary.

**romfs directory structure:**

```
romfs/
  bin/
    busybox               # The static ELF binary (~300 KB)
    sh -> busybox          # Symlinks — mkromfs stores as ROMFS_TYPE_SYMLINK
    ash -> busybox
    ls -> busybox
    cat -> busybox
    echo -> busybox
    ... (all enabled applets)
  sbin/
    init -> ../bin/busybox
  etc/
    hostname              # "ppap"
    motd                  # "Welcome to PicoPiAndPortable\n"
    inittab               # "::respawn:/bin/ash\n"
    fstab                 # mount table (Phase 5 entries)
    passwd                # "root:x:0:0:root:/root:/bin/ash\n"
    group                 # "root:x:0:\n"
    profile               # Shell startup: PATH, PS1, motd display
  dev/                    # Mount point (devfs overlays at boot)
  proc/                   # Mount point (procfs overlays at boot)
  tmp/                    # Mount point (tmpfs)
  mnt/
    sd/                   # Mount point (VFAT, Phase 4)
  root/                   # Root user's home directory
```

**`/etc/inittab`:**

```
# PPAP busybox init configuration
# Format: <id>:<runlevels>:<action>:<process>
::sysinit:/bin/mount -a
::respawn:/bin/ash
::ctrlaltdel:/bin/umount -a
::shutdown:/bin/umount -a
```

**`/etc/profile`:**

```sh
export PATH=/bin:/sbin:/usr/bin
export HOME=/root
export PS1='ppap# '
cat /etc/motd
```

**`/etc/passwd`:**

```
root:x:0:0:root:/root:/bin/ash
```

**`/etc/group`:**

```
root:x:0:
```

**mkromfs updates:**

The existing mkromfs handles symlinks (`ROMFS_TYPE_SYMLINK`).  The main
change is that the romfs image grows from ~2 KB to ~300–400 KB due to
the busybox binary.  Verify:
- The binary is 4-byte aligned (required for XIP Thumb execution)
- The total image fits in `FLASH_ROMFS` (16 MB available — no issue)
- The symlink farm resolves correctly via `mkromfs --dump`

**Build pipeline update (CMakeLists.txt):**

The CMake build invokes the third-party build scripts as custom commands:

```cmake
# Step 1: Build musl libc.a via third_party/build-musl.sh
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/musl-sysroot/lib/libc.a
  COMMAND ${CMAKE_SOURCE_DIR}/third_party/build-musl.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building musl libc.a for armv6m-thumb"
)

# Step 2: Build busybox via third_party/build-busybox.sh
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/busybox/busybox
  COMMAND ${CMAKE_SOURCE_DIR}/third_party/build-busybox.sh
  DEPENDS ${CMAKE_BINARY_DIR}/musl-sysroot/lib/libc.a
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building busybox with musl for armv6m-thumb"
)

# Step 3: Install busybox + symlinks into romfs/bin/
add_custom_command(
  OUTPUT ${ROMFS_DIR}/bin/busybox
  COMMAND ${CMAKE_COMMAND} -E copy
          ${CMAKE_BINARY_DIR}/busybox/busybox ${ROMFS_DIR}/bin/busybox
  COMMAND ${CMAKE_BINARY_DIR}/busybox/busybox --install -s ${ROMFS_DIR}/bin
  DEPENDS ${CMAKE_BINARY_DIR}/busybox/busybox
)
# Step 4: Run mkromfs to generate the image (existing, unchanged)
```

The build scripts handle patch application/revert internally, so repeated
`ninja` invocations are safe.  A `PPAP_SKIP_THIRDPARTY` cache variable
can bypass the musl/busybox rebuild for kernel-only development cycles.

**ELF loader update (`exec.c`):**

The current ELF loader allocates user pages for `.data`/`.bss` from the
page pool.  A 300 KB busybox binary has:
- `.text` + `.rodata`: ~280 KB (XIP from flash, no SRAM needed)
- `.data`: ~2–5 KB (copied to SRAM page)
- `.bss`: ~5–15 KB (zeroed in SRAM pages)

The `.data` + `.bss` requirement of ~10–20 KB needs 3–5 pages.  The current
loader handles up to 4 `user_pages` per process — extend to 6 if needed.

**Gotcha — large GOT:** busybox with PIC has a large Global Offset Table.
Ensure the GOT relocation loop in `exec.c` handles GOT entries that span
multiple pages.  Alternatively, if busybox is linked as `ET_EXEC` (fixed
address), no GOT relocation is needed — the binary runs at a fixed flash
address via XIP.

**XIP address for busybox:** since busybox lives at a known offset in romfs
(which is at a known flash address), the ELF can be linked at that address.
The mkromfs tool reports the data offset of each file; a post-processing
step patches the linker script or verifies alignment.  Simpler approach:
link busybox as ET_DYN (PIC) and let the loader compute addresses from the
romfs XIP base.

---

## Week 3: Syscall Coverage

### Step 7 — Process and Identity Syscalls

Implement the process management and user/group identity syscalls needed
by musl startup and busybox.

**New syscalls:**

| Syscall | Implementation |
|---|---|
| `exit_group(status)` (248) | Alias for `_exit()` — PPAP has no threads, so exit_group = exit |
| `getuid()` (199) | Return 0 (root) — single-user system |
| `geteuid()` (201) | Return 0 |
| `getgid()` (200) | Return 0 |
| `getegid()` (202) | Return 0 |
| `getppid()` (64) | Return `current->ppid` |
| `setpgid(pid, pgid)` (57) | Set `proc_table[pid].pgid`; add `pgid` field to PCB |
| `getpgid(pid)` (132) | Return `proc_table[pid].pgid` |
| `setsid()` (66) | Create new session: `current->sid = current->pid; current->pgid = current->pid` |
| `uname(buf)` (122) | Fill `struct utsname` with `"PicoPiAndPortable"`, `"armv6m"`, etc. |
| `set_tid_address(addr)` (256) | Store pointer (for musl startup); return `getpid()` |

**PCB extensions:**

```c
/* Add to pcb_t */
pid_t    pgid;          /* process group ID */
pid_t    sid;           /* session ID */
int     *clear_child_tid; /* set_tid_address pointer (musl startup) */
```

**`uname` output:**

```c
struct utsname {
    char sysname[65];    /* "PicoPiAndPortable" */
    char nodename[65];   /* "ppap" */
    char release[65];    /* "0.6.0" */
    char version[65];    /* build date */
    char machine[65];    /* "armv6m" */
};
```


### Step 8 — File and I/O Syscalls

Implement file-related syscalls that musl and busybox require.

**New syscalls:**

| Syscall | Implementation |
|---|---|
| `writev(fd, iov, cnt)` (146) | Iterate `iovec` array, call `sys_write` for each; musl stdio uses this |
| `ioctl(fd, req, arg)` (54) | Dispatch to file_ops->ioctl; tty handles TCGETS/TCSETS/TIOCGWINSZ |
| `fcntl(fd, cmd, arg)` (55) | `F_GETFD`/`F_SETFD` (close-on-exec), `F_GETFL`/`F_SETFL` (status flags), `F_DUPFD` |
| `access(path, mode)` (33) | `vfs_lookup` + check mode bits (PPAP is single-user root: always succeed for existing files) |
| `readlink(path, buf, sz)` (85) | `vfs_lookup` (no follow) → `ops->readlink` |
| `lstat(path, buf)` (107) | `vfs_lookup` with `LOOKUP_NOFOLLOW` flag → `ops->stat` |
| `umask(mask)` (60) | Store `current->umask`; return old value; used by file creation |
| `ftruncate(fd, len)` (93) | Truncate file — UFS/VFAT `ops->truncate`; romfs returns `-EROFS` |
| `link(old, new)` (9) | UFS `ops->link` (hard link); VFAT returns `-EPERM` |
| `symlink(target, path)` (83) | UFS `ops->symlink`; others return `-EPERM` |
| `rename(old, new)` (38) | VFS-routed rename (same FS only) |
| `mknod(path, mode, dev)` (14) | Stub: return `-EPERM` (devices created by devfs, not mknod) |

**`*at` syscall family:**

musl internally uses `openat`, `fstatat`, `readlinkat`, `faccessat`, etc.
These take a `dirfd` argument.  Implement by:
1. If `dirfd == AT_FDCWD` (-100): use `current->cwd` as base (same as non-`at` version)
2. If path is absolute: ignore `dirfd`
3. Otherwise: resolve relative to the directory referenced by `dirfd`

The `AT_FDCWD` fast path covers most musl usage.  Full `dirfd` resolution
is a stretch goal.

| Syscall | Number | Delegates to |
|---|---|---|
| `openat(dirfd, path, flags, mode)` | 322 | `sys_open` (with dirfd support) |
| `fstatat(dirfd, path, buf, flags)` | 327 | `sys_stat` / `sys_lstat` |
| `readlinkat(dirfd, path, buf, sz)` | 305 | `sys_readlink` |
| `faccessat(dirfd, path, mode, flags)` | 334 | `sys_access` |
| `unlinkat(dirfd, path, flags)` | 328 | `sys_unlink` / `sys_rmdir` |
| `mkdirat(dirfd, path, mode)` | 323 | `sys_mkdir` |
| `renameat(olddirfd, old, newdirfd, new)` | 329 | `sys_rename` |

**VFS updates for `LOOKUP_NOFOLLOW`:**

Add a `flags` parameter to `vfs_lookup()` to support `AT_SYMLINK_NOFOLLOW`.
When set, the final component of the path is not resolved if it is a symlink.
This is needed for `lstat` and `readlink`.

**termios / ioctl for tty (`tty.c`):**

ash checks terminal capabilities via `ioctl(fd, TCGETS, &termios)`.  Add
basic termios support to the tty driver:

```c
/* Minimal termios — stored per tty device */
struct termios {
    uint32_t c_iflag;     /* input: ICRNL, IGNCR */
    uint32_t c_oflag;     /* output: OPOST, ONLCR */
    uint32_t c_cflag;     /* control: CS8, B115200 */
    uint32_t c_lflag;     /* local: ECHO, ICANON, ISIG */
    uint8_t  c_cc[20];    /* control chars: VINTR=^C, VEOF=^D, VERASE=^H */
};

/* ioctl commands for tty */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
```

**TIOCGWINSZ:** return a fixed 80×24 window size (reasonable default for
UART terminal emulators).

**ISIG flag:** when `c_lflag & ISIG`, the tty driver intercepts Ctrl-C
(0x03) in the UART RX path and sends `SIGINT` to the foreground process
group.  Similarly, Ctrl-Z (0x1A) sends `SIGTSTP` (but job control
suspend is a stretch goal).


### Step 9 — Memory, Signal, and Time Syscalls

Complete the remaining syscall gaps.

**Anonymous mmap (`sys_mem.c`):**

musl's malloc uses `mmap(MAP_ANONYMOUS | MAP_PRIVATE)` for large allocations
(≥ 128 KB by default, but tunable).  On PPAP, anonymous mmap allocates
pages from the page pool:

```c
long sys_mmap2(long addr, long len, long prot, long flags,
               long fd, long pgoff) {
    if (fd != -1 || !(flags & MAP_ANONYMOUS))
        return -ENOSYS;  /* only anonymous mappings supported */

    uint32_t pages_needed = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages_needed > 4) return -ENOMEM;  /* limit per-mapping size */

    /* Allocate contiguous pages (best-effort) */
    void *base = page_alloc_contiguous(pages_needed);
    if (!base) return -ENOMEM;

    if (flags & MAP_PRIVATE)
        memset(base, 0, pages_needed * PAGE_SIZE);

    /* Track in PCB for cleanup on exit */
    mmap_region_add(current, base, pages_needed);
    return (long)base;
}

long sys_munmap(long addr, long len) {
    uint32_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    mmap_region_remove(current, (void *)addr, pages);
    for (uint32_t i = 0; i < pages; i++)
        page_free((uint8_t *)addr + i * PAGE_SIZE);
    return 0;
}
```

**Note:** musl's malloc threshold can be configured at link time via
`#define MMAP_THRESHOLD 0x7fffffff` to force all allocations through
`brk()`/`sbrk()`, avoiding mmap entirely.  This is the simpler approach
if page pool pressure is a concern.  Try mmap first; fall back to
brk-only if pool exhaustion is observed.

**Signal mask syscalls (`sys_signal.c`):**

```c
/* Add to PCB: */
uint32_t sig_mask;      /* blocked signals bitmask */
uint32_t sig_pending;   /* pending signals bitmask (already exists) */

long sys_rt_sigprocmask(long how, long set_ptr, long old_ptr, long sigsetsize) {
    uint32_t old = current->sig_mask;
    if (old_ptr) *(uint32_t *)old_ptr = old;

    if (set_ptr) {
        uint32_t set = *(const uint32_t *)set_ptr;
        switch (how) {
            case SIG_BLOCK:   current->sig_mask |= set; break;
            case SIG_UNBLOCK: current->sig_mask &= ~set; break;
            case SIG_SETMASK: current->sig_mask = set; break;
            default: return -EINVAL;
        }
        /* SIGKILL and SIGSTOP cannot be blocked */
        current->sig_mask &= ~((1u << SIGKILL) | (1u << SIGSTOP));
    }
    return 0;
}
```

**Time syscalls:**

```c
/* PPAP has no RTC.  Time starts at 0 at boot and increments via SysTick.
 * A static boot_ticks counter is maintained by SysTick_Handler. */
static volatile uint32_t boot_ticks;  /* incremented every 10 ms */

long sys_clock_gettime(long clk_id, long tp_ptr) {
    struct timespec *tp = (struct timespec *)tp_ptr;
    uint32_t ticks = boot_ticks;
    tp->tv_sec  = ticks / 100;
    tp->tv_nsec = (ticks % 100) * 10000000;  /* 10 ms resolution */
    return 0;
}

long sys_gettimeofday(long tv_ptr, long tz_ptr) {
    struct timeval *tv = (struct timeval *)tv_ptr;
    uint32_t ticks = boot_ticks;
    tv->tv_sec  = ticks / 100;
    tv->tv_usec = (ticks % 100) * 10000;
    return 0;
}

long sys_times(long buf_ptr) {
    struct tms *buf = (struct tms *)buf_ptr;
    buf->tms_utime  = boot_ticks;  /* approximate */
    buf->tms_stime  = 0;
    buf->tms_cutime = 0;
    buf->tms_cstime = 0;
    return (long)boot_ticks;
}
```

**Catch-all for unimplemented syscalls:**

The dispatch table returns `-ENOSYS` for any syscall number not in the
table.  Add a debug print (gated by a `#define SYSCALL_DEBUG`) that logs
the unimplemented syscall number — invaluable during busybox bring-up:

```c
if (nr >= SYSCALL_MAX || !syscall_table[nr]) {
#ifdef SYSCALL_DEBUG
    uart_puts("ENOSYS: syscall ");
    uart_print_dec(nr);
    uart_puts("\n");
#endif
    return -ENOSYS;
}
```

---

## Week 4: Init, Terminal, and Interactive Shell

### Step 10 — TTY Line Discipline and Terminal Control

The tty driver needs a minimal line discipline for interactive use.
ash relies on the terminal being in "raw" or "cooked" mode and uses
termios ioctls to switch.

**Canonical (cooked) mode (`c_lflag & ICANON`):**

In canonical mode the tty driver buffers input line-by-line:
- Characters are echoed if `ECHO` is set
- Backspace (0x7F or 0x08) deletes the last character
- Enter (0x0D or 0x0A) completes the line and wakes `read()`
- Ctrl-D (0x04) at the start of a line signals EOF (read returns 0)
- Ctrl-C (0x03) sends SIGINT if `ISIG` is set

**Raw mode (no `ICANON`):**

In raw mode, each character is immediately available to `read()`:
- No line buffering
- No echo (unless `ECHO` is still set)
- No special character processing (except ISIG signals if set)

ash switches to raw mode for command-line editing and back to cooked
mode before running external commands.

**Implementation in `tty.c`:**

```c
/* Line buffer for canonical mode */
#define TTY_LINE_MAX  256
static char line_buf[TTY_LINE_MAX];
static uint16_t line_pos;
static volatile uint8_t line_ready;  /* set when \n received */

/* Current terminal settings */
static struct termios tty_termios = {
    .c_iflag = ICRNL,           /* CR → NL */
    .c_oflag = OPOST | ONLCR,  /* NL → CR+NL */
    .c_cflag = CS8 | B115200,
    .c_lflag = ECHO | ICANON | ISIG,
    .c_cc = {
        [VINTR]  = 0x03,   /* Ctrl-C */
        [VEOF]   = 0x04,   /* Ctrl-D */
        [VERASE] = 0x7F,   /* DEL */
        [VKILL]  = 0x15,   /* Ctrl-U */
        [VMIN]   = 1,
        [VTIME]  = 0,
    },
};
```

**RX path update:** the UART RX interrupt handler feeds characters through
the line discipline before placing them in the read buffer.  In canonical
mode, characters go into `line_buf` first; in raw mode, they go directly
to the read ring buffer.

**Process group and foreground job:**

The tty tracks a foreground process group (`tty_fg_pgid`).  When ISIG is
set and Ctrl-C arrives, `SIGINT` is sent to all processes in the foreground
group.  `ioctl(TIOCSPGRP)` sets the foreground group (called by ash during
job control).


### Step 11 — /sbin/init and Boot to Shell

Connect the kernel's boot sequence to launch busybox init as PID 1.

**Kernel launch sequence (end of `kmain()`):**

```c
/* After all filesystem mounts (romfs, devfs, procfs, VFAT, UFS loopbacks): */

/* Launch /sbin/init as PID 1 */
pcb_t *init = proc_alloc();
if (!init) { uart_puts("PANIC: cannot allocate init process\n"); for(;;); }

fd_stdio_init(init);   /* fd 0/1/2 → tty */
strcpy(init->cwd, "/");
init->pgid = init->pid;
init->sid  = init->pid;

int err = do_execve(init, "/sbin/init");
if (err < 0) {
    uart_puts("WARN: /sbin/init failed, trying /bin/ash\n");
    err = do_execve(init, "/bin/ash");
}
if (err < 0) {
    uart_puts("PANIC: no init or shell found\n");
    for (;;);
}

sched_start();  /* start scheduler — init runs */
/* kmain never returns */
```

**busybox init behavior:**

1. busybox init reads `/etc/inittab`
2. Executes `::sysinit:` entries (e.g., `mount -a` to mount all fstab entries)
3. Spawns `::respawn:` entries (e.g., `/bin/ash` on the console)
4. If ash exits, init respawns it (respawn action)
5. On shutdown signals, runs `::shutdown:` entries

**`/etc/inittab` for PPAP:**

```
# Run mount at boot (mounts /mnt/sd, /usr, /home, /var from fstab)
::sysinit:/bin/mount -a
# Spawn ash on the console with login-style startup
::respawn:-/bin/ash
# Clean unmount on shutdown
::shutdown:/bin/umount -a
```

The `-/bin/ash` prefix tells init to invoke ash as a login shell (reads
`/etc/profile`).

**Fallback if init is missing:** if `/sbin/init` is not found (e.g.,
during early development without full busybox), the kernel falls back to
launching `/bin/ash` directly.  This provides a shell even without init.

**QEMU testing:** the same init sequence works on QEMU.  The `mount -a`
step may fail (no SD card on QEMU) but init continues to spawn ash.  On
QEMU, the serial console is the CMSDK UART.


### Step 12 — Integration Testing and Hardening

Systematic testing of the complete boot-to-shell flow.

**Test matrix:**

| Test | Expected Result | Platform |
|---|---|---|
| Boot to `ppap# ` prompt | init spawns ash, profile prints motd + sets PS1 | QEMU + HW |
| `echo hello` | Prints "hello" | QEMU + HW |
| `cat /etc/hostname` | Prints "ppap" | QEMU + HW |
| `cat /etc/passwd` | Prints "root:x:0:0:root:/root:/bin/ash" | QEMU + HW |
| `uname -a` | "PicoPiAndPortable ppap 0.6.0 ... armv6m" | QEMU + HW |
| `echo hello \| cat` | Pipeline: prints "hello" | QEMU + HW |
| `echo hello > /tmp/test; cat /tmp/test` | Redirect to tmpfs file | QEMU + HW |
| `kill -0 1` | Success (init is alive) | QEMU + HW |
| Ctrl-C during `sleep 100` | Interrupts sleep, returns to prompt | QEMU + HW |
| `ls /mnt/sd` | Lists files on FAT32 SD card | HW only |
| `cat /mnt/sd/somefile.txt` | Reads file from SD | HW only |
| Exit ash (Ctrl-D) | init respawns ash — new prompt appears | QEMU + HW |

> **Note:** Some tests originally planned here were deferred:
>
> - `ls /`, `ls /bin` — blocked by the getdents infinite loop bug;
>   fixed and verified in **Step 13**.
> - `ps`, `top`, `free`, `df`, concurrent process listing — require procfs
>   per-PID directories not yet implemented. Covered in **Step 14**.

**Common failure modes and debugging:**

| Symptom | Likely Cause | Fix |
|---|---|---|
| "ENOSYS: syscall N" in debug log | Missing syscall | Implement or add stub returning 0/-ENOSYS |
| ash crashes on startup | Missing termios ioctl | Ensure TCGETS returns valid termios |
| No output after exec busybox | fd 0/1/2 not inherited | Verify fd table copy in vfork/exec path |
| Printf produces no output | musl's writev not implemented | Implement sys_writev |
| "can't access tty" error | /dev/ttyS0 not accessible from ash | Ensure init opens /dev/ttyS0 for fd 0/1/2 |
| Symlinks not resolved | VFS symlink loop or romfs bug | Test with `readlink /bin/ls`, verify romfs dump |
| mmap fails, malloc returns NULL | Anonymous mmap not implemented | Implement sys_mmap2 or force brk-only malloc |
| Ctrl-C doesn't work | ISIG not set or SIGINT not delivered to pgid | Check tty_termios.c_lflag, tty_fg_pgid |

**Performance baseline:**

Measure boot time from power-on to shell prompt:
- Target: < 2 seconds (spec §7)
- Breakdown: stage1 (< 10 ms) + kernel init (< 50 ms) + romfs mount (< 1 ms)
  + SD init (200–500 ms) + VFAT mount (< 50 ms) + loopback mounts (< 100 ms)
  + exec init (< 10 ms) + exec ash (< 10 ms)

**Memory usage baseline:**

After boot to ash prompt:
- Kernel data: ~15 KB (kernel BSS, globals)
- Process pages: init (1 stack + 1 data page) + ash (1 stack + 3–5 data pages)
  ≈ 6–8 pages = 24–32 KB
- Free pages: 52 − 8 = ~44 pages = 176 KB remaining for user processes

---

## Week 5: Debugging, Stability, and Extended Applets

### Step 13 — Memory and Readdir Bug Fixes

Debug session that resolved "ls: out of memory" failure when running
`ls /` from busybox ash.  Three root causes were identified and fixed:

**Root cause 1: getdents infinite loop (primary)**

`romfs_readdir()` uses byte-offset cookies where `*cookie == 0` means
"start from first child."  When the last child's `next_off` is 0 (end of
sibling chain), the cookie resets to 0 — indistinguishable from "start
over."  This caused `sys_getdents64` to endlessly re-read the same
directory entries, with busybox `ls` allocating memory for each duplicate
batch until OOM.

Fix: added a `GETDENTS_EOF` sentinel (`0xFFFFFFFF`) in `sys_fs.c`.  When
`readdir` returns entries but sets cookie to 0, `sys_getdents64` stores
`GETDENTS_EOF` in `f->offset`.  Subsequent calls return 0 immediately.
Applied to both `sys_getdents` and `sys_getdents64`.

Only romfs had this bug — devfs, procfs, tmpfs, vfat, and ufs all use
sequential integer indices for cookies.

**Root cause 2: brk Linux semantics violation**

`sys_brk()` was returning `-(long)ENOMEM` on failure.  Linux brk always
returns the current program break — on failure it returns the *unchanged*
break (never a negative errno).  musl relies on this: it calls brk, checks
whether the return value changed, and falls back to mmap if it didn't.
With a negative return, musl interpreted it as a valid (huge) address and
never fell back to mmap.

Fix: `sys_brk()` now returns `(long)current->brk_current` on all failure
paths, matching Linux semantics.

**Root cause 3: insufficient QEMU page pool**

The page allocator was hardcoded to 51 pages (204 KB) — sized for RP2040's
264 KB SRAM.  QEMU's mps2-an500 has 512 KB, so we can afford more.
Additionally, `SRAM_IOBUF_BASE` and `SRAM_DMA_BASE` were hardcoded
addresses that didn't move when `PAGE_COUNT` changed.

Fix: conditional `PAGE_COUNT` in `config.h` (96 for QEMU, 51 for RP2040).
Made `SRAM_IOBUF_BASE` and `SRAM_DMA_BASE` computed from pool end in
`page.h`.

**Additional fixes:**

- `MMAP_REGIONS_MAX` raised from 4 to 8 (musl malloc can use many mmap
  regions simultaneously)
- `sys_exit()` now frees all mmap regions on process termination (was
  leaking pages)

**Files modified:**

| File | Change |
|---|---|
| `src/config.h` | Conditional `PAGE_COUNT` (96 QEMU / 51 RP2040), `MMAP_REGIONS_MAX=8` |
| `src/kernel/mm/page.h` | Computed `SRAM_IOBUF_BASE`/`SRAM_DMA_BASE` from pool end |
| `src/kernel/proc/proc.h` | `mmap_regions[MMAP_REGIONS_MAX]` |
| `src/kernel/syscall/sys_mem.c` | brk Linux semantics fix, use `MMAP_REGIONS_MAX` |
| `src/kernel/syscall/sys_fs.c` | `GETDENTS_EOF` sentinel in `sys_getdents`/`sys_getdents64` |
| `src/kernel/syscall/sys_proc.c` | mmap region cleanup in `sys_exit` |

**Verification:**

```
$ ls /
bin     dev     etc     mnt     proc    root    sbin    tmp

$ ls /bin
ash        busybox    cat        echo       grep       kill       ls
mkdir      mount      ps         rm         rmdir      sleep      top
umount

$ cat /etc/hostname
ppap
```


### Step 14 — Enable ps and top Applets (procfs Extensions)

Enable busybox `ps` and `top` applets by extending procfs with per-process
entries and adding CPU/memory accounting fields to the PCB.

**busybox ps requirements:**

busybox `ps` (via `libbb/procps.c`) reads:
- `/proc/<pid>/stat` — PID, comm, state, ppid, pgid, sid, tty, utime, stime, vsz, rss
- `/proc/<pid>/cmdline` — NUL-separated argv (for process name display)
- `/proc/` directory listing — to enumerate all PIDs

**busybox top additional requirements:**

- `/proc/stat` — aggregate CPU jiffy counters (user, nice, system, idle)
- `/proc/uptime` — seconds since boot
- `/proc/meminfo` — memory stats (already implemented)

**PCB extensions:**

```c
/* Add to pcb_t in proc.h */
char     comm[16];       /* command name (basename of executable) */
uint32_t utime;          /* user-mode ticks consumed */
uint32_t stime;          /* kernel-mode ticks consumed */
uint32_t start_time;     /* boot tick when process was created */
```

- `comm`: set during `do_execve()` from the basename of the executable path
- `utime`/`stime`: incremented in the SysTick handler based on whether the
  interrupted context was Thread mode (user) or Handler mode (kernel)
- `start_time`: set to the global tick counter at `proc_alloc()` time

**Global kernel counters:**

```c
/* Kernel-wide jiffy counters for /proc/stat */
uint32_t cpu_user_ticks;    /* ticks in user mode */
uint32_t cpu_system_ticks;  /* ticks in kernel mode */
uint32_t cpu_idle_ticks;    /* ticks in idle (no runnable process) */
```

Updated in `sched_tick()` alongside the existing preemption logic.

**New procfs entries:**

| Path | Content |
|---|---|
| `/proc/<pid>/` | Directory per live process (enumerated by scanning proc_table) |
| `/proc/<pid>/stat` | `<pid> (<comm>) <state> <ppid> <pgid> <sid> <tty> 0 0 0 0 0 <utime> <stime> 0 0 0 <nice> 1 0 <start_time> <vsz> <rss> ...` |
| `/proc/<pid>/cmdline` | `<comm>\0` (simplified: just the command name with NUL terminator) |
| `/proc/stat` | `cpu <user> 0 <system> <idle> 0 0 0 0 0 0` |
| `/proc/uptime` | `<seconds>.<hundredths> <idle_seconds>.<hundredths>` |

The `/proc/<pid>/stat` format follows Linux's 52-field format. Fields not
tracked are filled with zeros. busybox's parser extracts only the fields
it needs.

**procfs implementation changes:**

The current procfs is flat (only `/proc/meminfo` and `/proc/version`).
Extend it to support dynamic per-PID directories:

1. `procfs_readdir()` on root: enumerate fixed entries (`meminfo`,
   `version`, `stat`, `uptime`) plus one directory entry per live process
2. `procfs_lookup()` on root: parse numeric names to find matching PID
3. `procfs_readdir()` on `<pid>` dir: return `stat` and `cmdline` entries
4. `procfs_read()` on `<pid>/stat`: format the stat line from PCB fields
5. `procfs_read()` on `<pid>/cmdline`: return `comm` with NUL terminator

**State mapping:**

| `proc_state_t` | Linux char |
|---|---|
| `PROC_RUNNABLE` | `R` |
| `PROC_BLOCKED` / `PROC_SLEEPING` | `S` |
| `PROC_ZOMBIE` | `Z` |

**vsz/rss calculation:**

- `vsz` = (number of non-NULL `user_pages[]` + mmap pages + 1 stack page) × `PAGE_SIZE`
- `rss` = same as vsz (no swapping on RP2040)

**Busybox config changes:**

Add to `third_party/configs/busybox_ppap.fragment`:
```
CONFIG_PS=y
CONFIG_TOP=y
CONFIG_FEATURE_TOP_CPU_USAGE_PERCENTAGE=y
CONFIG_FEATURE_TOP_CPU_GLOBAL_PERCENTS=y
```

Rebuild busybox and regenerate the romfs image to include `ps` and `top`
symlinks.

**Files modified:**

| File | Change |
|---|---|
| `src/kernel/proc/proc.h` | Add `comm[16]`, `utime`, `stime`, `start_time` to PCB |
| `src/kernel/proc/sched.c` | Increment `utime`/`stime`/global CPU counters in `sched_tick()` |
| `src/kernel/proc/proc.c` | Set `start_time` in `proc_alloc()` |
| `src/kernel/exec/exec.c` | Set `comm` from executable basename in `do_execve()` |
| `src/kernel/fs/procfs.c` | Per-PID directories, `/proc/<pid>/stat`, `/proc/<pid>/cmdline`, `/proc/stat`, `/proc/uptime` |
| `third_party/configs/busybox_ppap.fragment` | Enable `CONFIG_PS`, `CONFIG_TOP` |

**Verification:**

```
$ ps
  PID   Uid       VSZ Stat Command
    1     0     16384 S    init
    2     0     16384 S    ash
    3     0      4096 R    ps

$ top -b -n1
Mem: 384K used, 0K free, 0K shrd, 0K buff, 0K cached
CPU:   0% usr   0% sys   0% nic 100% idle   0% io   0% irq   0% sirq
  PID  PPID USER     STAT   VSZ %VSZ  %CPU COMMAND
    1     0 root     S    16384   4%   0%  init
    2     1 root     S    16384   4%   0%  ash
    3     2 root     R     4096   1%   0%  top
```

**Follow-up test matrix** (deferred from Step 12):

| Test | Expected Result | Platform |
|---|---|---|
| `ps` | Lists running processes (init, ash) | QEMU + HW |
| `free` | Shows memory usage (total/used/free) | QEMU + HW |
| `df` | Shows mounted filesystems and space (requires `sys_statfs`) | QEMU + HW |
| 3 concurrent processes (`sleep 10 & sleep 10 & ps`) | ps shows 5 processes (init, ash, 2×sleep, ps) | QEMU + HW |


### Step 15 — Enable mount, umount, df, and vi Applets ✓

Enable user-space `mount`/`umount` commands, the `df` disk-space utility,
and the `vi` text editor.  The VFS mount infrastructure already exists in
the kernel; this step adds the syscall interface, per-filesystem statfs
callbacks, and `/proc/mounts` so busybox can query and manage mounts.

**Status: Complete**

**New syscalls:**

| Syscall | Number | Signature |
|---|---|---|
| `SYS_MOUNT` | 21 | `mount(source, target, fstype, flags, data)` |
| `SYS_UMOUNT2` | 52 | `umount2(target, flags)` |
| `SYS_STATFS64` | 266 | `statfs64(path, sz, buf)` |
| `SYS_FSTATFS64` | 267 | `fstatfs64(fd, sz, buf)` |

**Implementation details:**

- `sys_mount()`: maps fstype string → vfs_ops_t (devfs, procfs, tmpfs,
  vfat, ufs), strips `/dev/` prefix for blkdev lookup, converts
  `MS_RDONLY` → `MNT_RDONLY`, calls `vfs_mount()`
- `sys_umount2()`: normalizes path, calls `vfs_umount()` which checks
  for busy vnodes before deactivating the mount entry
- `sys_statfs64()` / `sys_fstatfs64()`: find mount entry via
  `vfs_find_mount()` or fd→vnode→mount chain, call `ops->statfs()`
- Added `statfs` callback to `vfs_ops_t` and implemented it in all 6
  filesystems (romfs, devfs, procfs, tmpfs, vfat, ufs) with correct
  Linux magic numbers
- Added `/proc/mounts` to procfs — `gen_mounts()` iterates
  `vfs_mount_table` and outputs Linux-format mount lines; required by
  busybox `mount` (no args) and `df`

**Bug fix — ENOMEM on RP2040 init:**

busybox data segment (.got + .data + .bss) grew to 34 KB (9 pages) with
the new applets, exceeding the hardcoded 8-page `user_pages[]` limit.
Introduced `USER_PAGES_MAX = 12` in `config.h` and updated all iteration
sites (exec.c, sys_proc.c, sys_mem.c, procfs.c).

**Other fix — QEMU "0/1" output noise:**

Removed obsolete Phase 1 context-switch test threads from main_qemu.c.
Thread 0 now idles with `wfi` (matching the real board).

**busybox config additions:**

```
CONFIG_MOUNT=y
CONFIG_UMOUNT=y
CONFIG_DF=y
CONFIG_VI=y
CONFIG_FEATURE_VI_COLON=y
CONFIG_FEATURE_VI_SEARCH=y
```

**Files modified:**

| File | Change |
|---|---|
| `src/kernel/vfs/vfs.h` | Add `struct kernel_statfs`, `statfs` in `vfs_ops_t`, `vfs_umount()` decl |
| `src/kernel/vfs/vfs.c` | Implement `vfs_umount()` |
| `src/kernel/fs/romfs.c` | Add `romfs_statfs` |
| `src/kernel/fs/devfs.c` | Add `devfs_statfs` |
| `src/kernel/fs/procfs.c` | Add `procfs_statfs`, `/proc/mounts` via `gen_mounts()` |
| `src/kernel/fs/tmpfs.c` | Add `tmpfs_statfs` |
| `src/kernel/fs/vfat.c` | Add `vfat_statfs` (scans FAT for free clusters) |
| `src/kernel/fs/ufs.c` | Add `ufs_statfs` |
| `src/kernel/syscall/syscall.h` | Add 4 syscall numbers + declarations |
| `src/kernel/syscall/syscall.c` | Dispatch entries for all 4 syscalls |
| `src/kernel/syscall/sys_fs.c` | Implement `sys_mount`, `sys_umount2`, `sys_statfs64`, `sys_fstatfs64` |
| `src/config.h` | Add `USER_PAGES_MAX = 12` |
| `src/kernel/proc/proc.h` | `user_pages[USER_PAGES_MAX]` (was `[8]`) |
| `src/kernel/exec/exec.c` | Use `USER_PAGES_MAX` for cap + cleanup |
| `src/kernel/syscall/sys_proc.c` | Use `USER_PAGES_MAX` in exit/vfork/execve |
| `src/kernel/syscall/sys_mem.c` | Use `USER_PAGES_MAX` in brk |
| `src/kernel/main_qemu.c` | Remove "0/1" test threads, idle with wfi |
| `third_party/configs/busybox_ppap.fragment` | Enable mount/umount/df/vi |
| `third_party/install-busybox.sh` | Add df, vi, mount, umount symlinks |

### Step 16 — General Blocking I/O and poll Syscall ✓

**Status: Complete** (core implementation done; Ctrl-C signal delivery to
foreground process still under investigation)

Commits:
- `ad10e45` Phase 6 Step 16: blocking I/O, poll syscall, signal-aware nanosleep
- `1d3e9ed` Fix signal delivery infrastructure, POSIX exec signal reset

**Implemented:**
- Non-spinning tty_read (PROC_BLOCKED + svc_restart pattern)
- Signal-aware nanosleep (svc_restart, returns -EINTR on pending signal)
- ppoll / ppoll_time64 syscalls (SYS_PPOLL 336, SYS_PPOLL_TIME64 414)
- poll callback in file_ops (tty, pipe, devnull, procfs)
- UART ISR Ctrl-C detection → tty_signal_intr → tty_send_signal
- tty_send_signal wakes PROC_BLOCKED and PROC_SLEEPING processes
- sched_wakeup triggers PendSV for prompt context switch
- tty_set_fg_pgrp API; set tty_fg_pgrp = init->pid at boot
- POSIX exec signal reset (caught handlers → SIG_DFL, clear sig_pending/sig_blocked)
- SYSCALL_DEBUG tracing infrastructure (filtered, QEMU-only)
- Kernel tests: 11 blocking I/O tests (ppoll, signal delivery, nanosleep)
- User-space tests: test_poll, test_sleep_intr

**Known issue:** Ctrl-C does not yet reliably interrupt `sleep` or `top`
from the interactive shell. Signal delivery path (tty_fg_pgrp matching,
process wakeup, signal_check → SIG_DFL termination) needs further debugging.
All 110 kernel tests pass.

---

Three blocking I/O problems remain after Steps 1–15:

1. **tty_read busy-waits** (WFI loop in SVC handler) — prevents signal delivery to the blocked process and wastes CPU scheduling time.
2. **Ctrl-C can't interrupt sleeping processes** — `tty_send_signal()` sets `sig_pending` but doesn't wake `PROC_SLEEPING`/`PROC_BLOCKED` processes (unlike `sys_kill()` which does).
3. **No poll/select** — busybox `top` uses `poll()` for keyboard input; any program that checks fd readiness before reading has no way to do so.

All three share a root cause: the kernel lacks a general mechanism for blocking on I/O events with proper signal interruption and readiness queries.

**Design: extend the existing PROC_BLOCKED + svc_restart pattern to all I/O sources**

The pipe driver already implements correct non-spinning blocking I/O:
process marks `PROC_BLOCKED` + `svc_restart=1`, the other end calls
`sched_wakeup(channel)`, and the syscall re-executes on wakeup.  This
step extends that same pattern to TTY read, poll, and nanosleep.

---

**A. Convert tty_read to non-spinning blocking**

Replace the WFI busy-wait with the pipe-style block pattern:

```c
// tty_read_canon: when uart_getc() returns -1 (no data)
if (c < 0) {
    if (current->sig_pending & ~current->sig_blocked)
        return -(long)EINTR;       // signal arrived — don't block
    current->wait_channel = &tty_stdin;
    current->state = PROC_BLOCKED;
    svc_restart = 1;
    sched_yield();
    return 0;                      // ignored — svc_restart re-executes
}
```

This works because `line_buf` and `line_pos` are static — state is
preserved across re-entries.  Each wakeup processes all buffered chars,
then re-blocks if the line isn't complete.

Raw mode (`tty_read_raw`) gets the same treatment: block when no data
and `got == 0`.

**B. UART ISR: data-arrival wakeup + inline Ctrl-C detection**

Add to `UART0RX_IRQ_Handler` (both `uart_qemu.c` and `uart.c`):

1. After buffering each byte, call `sched_wakeup(&tty_stdin)` to wake
   any process blocked on tty read.
2. If the byte is 0x03 (Ctrl-C) and the `tty_isig` flag is set, call
   `tty_send_signal(SIGINT)` directly from the ISR.  This delivers
   SIGINT even when no process is reading the TTY (e.g., during
   `sleep 10`).

New `uart_rx_avail()` function returns non-zero if the RX ring buffer
is non-empty (for poll readiness without consuming data).

**C. Fix tty_send_signal to wake blocked/sleeping processes**

Align with `sys_kill()` — after setting `sig_pending`, also set
`PROC_RUNNABLE` if the target is `PROC_BLOCKED` or `PROC_SLEEPING`:

```c
static void tty_send_signal(int sig)
{
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].state != PROC_FREE &&
            (int32_t)proc_table[i].pgid == tty_fg_pgrp) {
            proc_table[i].sig_pending |= (1u << (uint32_t)sig);
            if (proc_table[i].state == PROC_BLOCKED ||
                proc_table[i].state == PROC_SLEEPING)
                proc_table[i].state = PROC_RUNNABLE;
        }
    }
}
```

**D. Signal-aware nanosleep (return -EINTR)**

Convert `sys_nanosleep` / `sys_clock_nanosleep*` to use `svc_restart`:

```c
long sys_nanosleep(void *req, void *rem)
{
    // Restart detection: sleep_until still set from previous call
    if (current->sleep_until != 0) {
        if ((int32_t)(sched_get_ticks() - current->sleep_until) >= 0) {
            current->sleep_until = 0;
            return 0;                  // timer expired normally
        }
        if (current->sig_pending & ~current->sig_blocked) {
            current->sleep_until = 0;
            return -(long)EINTR;       // interrupted by signal
        }
        // Spurious wakeup — re-sleep
        current->state = PROC_SLEEPING;
        svc_restart = 1;
        sched_yield();
        return 0;
    }
    // First call — compute ticks, start sleeping
    sched_sleep(ticks);
    svc_restart = 1;
    return 0;
}
```

On wakeup the syscall re-executes via `svc_restart`.  The restart
path checks whether the timer expired (return 0) or a signal is
pending (return -EINTR).  SVC_Handler then calls `signal_check()` on
the RUNNABLE-return path, delivering the signal after the syscall.

**E. Add `poll` callback to file_ops**

```c
// file.h
struct file_ops {
    long  (*read) (struct file *f, char *buf, size_t n);
    long  (*write)(struct file *f, const char *buf, size_t n);
    int   (*close)(struct file *f);
    int   (*ioctl)(struct file *f, uint32_t cmd, void *arg);
    short (*poll) (struct file *f, short events);   // NEW
};
```

Implementations:

| Source | POLLIN | POLLOUT | POLLHUP / POLLERR |
|---|---|---|---|
| tty (read fd) | `uart_rx_avail() > 0` or `line_ready` (ICANON) | — | never |
| tty (write fd) | — | always | never |
| pipe read end | buffer non-empty OR `writers == 0` | — | `writers == 0` → POLLHUP |
| pipe write end | — | buffer has space OR `readers == 0` | `readers == 0` → POLLERR |
| regular file | always | always | never |
| procfs / devfs | always | always | never |

A NULL `poll` pointer means "always ready" (POLLIN\|POLLOUT).

**F. sys_ppoll / sys_ppoll_time64**

New file `src/kernel/syscall/sys_poll.c`.

```
long sys_ppoll_time64(struct pollfd *fds, uint32_t nfds,
                      const struct timespec64 *tmo, const void *sigmask)
```

Algorithm:
1. Scan all `nfds` entries: look up `fd_table[fd]`, call `poll()` callback
2. If any revents non-zero → return count of ready fds
3. If `tmo` is zero-duration → return 0 (non-blocking)
4. Block: set `PROC_BLOCKED`, `wait_channel = &poll_wake_sentinel`,
   `svc_restart = 1`.  If timeout > 0, set `sleep_until` deadline.
5. On wakeup (svc_restart re-executes): re-scan fds.
   If ready → return.  If sig_pending → return -EINTR.
   If timeout expired → return 0.  Else re-block.

All I/O event sources (`sched_wakeup(&tty_stdin)`, `sched_wakeup(pipe)`,
etc.) also call `sched_wakeup(&poll_wake_sentinel)` to wake ALL
poll-blocked processes.  With max 8 processes, the "thundering herd"
cost is negligible.

musl calls `SYS_ppoll_time64` (414) first, then `SYS_ppoll` (336).
Implement both; the 32-bit variant just converts the timespec.

**G. Timeout support for PROC_BLOCKED in sched_tick**

Add to `sched_tick()`:

```c
if (p->state == PROC_BLOCKED
        && p->sleep_until != 0
        && (int32_t)(tick_count - p->sleep_until) >= 0)
    p->state = PROC_RUNNABLE;
```

Existing PROC_BLOCKED uses (pipe, waitpid, vfork) have `sleep_until=0`,
so this check is a no-op for them.  Only poll-with-timeout sets both
`PROC_BLOCKED` and `sleep_until`.

---

**New syscalls:**

| Syscall | Number | Signature |
|---|---|---|
| `SYS_PPOLL` | 336 | `ppoll(fds, nfds, tmo_p, sigmask, sigsetsize)` |
| `SYS_PPOLL_TIME64` | 414 | `ppoll_time64(fds, nfds, tmo_p, sigmask, sigsetsize)` |

**Poll event constants (Linux-compatible):**

| Constant | Value | Meaning |
|---|---|---|
| `POLLIN` | 0x0001 | data available for reading |
| `POLLOUT` | 0x0004 | writing possible |
| `POLLERR` | 0x0008 | error condition |
| `POLLHUP` | 0x0010 | hung up |
| `POLLNVAL` | 0x0020 | invalid fd |

**Files modified:**

| File | Change |
|---|---|
| `src/kernel/fd/file.h` | Add `poll` to `file_ops` |
| `src/kernel/fd/tty.c` | Non-spinning read, poll callback, fix `tty_send_signal`, export `tty_isig` flag |
| `src/kernel/fd/tty.h` | Declare `tty_send_signal()` for UART ISR |
| `src/kernel/fd/pipe.c` | Poll callback, add `sched_wakeup(&poll_wake_sentinel)` calls |
| `src/drivers/uart_qemu.c` | Add `uart_rx_avail()`, ISR wakeup + Ctrl-C detection |
| `src/drivers/uart.c` | Same as `uart_qemu.c` |
| `src/drivers/uart.h` | Declare `uart_rx_avail()` |
| `src/kernel/syscall/sys_poll.c` | **New**: `sys_ppoll`, `sys_ppoll_time64` |
| `src/kernel/syscall/sys_time.c` | nanosleep/clock_nanosleep with svc_restart + -EINTR |
| `src/kernel/syscall/syscall.h` | Add `SYS_PPOLL` (336), `SYS_PPOLL_TIME64` (414), poll constants |
| `src/kernel/syscall/syscall.c` | Dispatch entries for ppoll/ppoll_time64 |
| `src/kernel/proc/sched.c` | PROC_BLOCKED+timeout in `sched_tick`, export `poll_wake_sentinel` |
| `src/kernel/proc/sched.h` | Declare `poll_wake_sentinel` |
| `CMakeLists.txt` | Add `sys_poll.c` to build |

**Verification:**

```
# Ctrl-C interrupts sleep
$ sleep 100
^C
$                      ← shell prompt returns immediately

# top responds to 'q'
$ top
  PID ... CPU%
    1 ... 0.0%
(press q)
$                      ← returns to shell

# Interactive shell remains responsive during background work
```

---

---

## Deliverables

| File | Description |
|---|---|
| `third_party/musl/` | git submodule: musl libc v1.2.5 |
| `third_party/busybox/` | git submodule: busybox 1.36.1 |
| `third_party/patches/musl/0001-*.patch` | PPAP SVC syscall wrappers for `syscall_arch.h` |
| `third_party/patches/musl/0002-*.patch` | Cortex-M0+ compatible atomics for `atomic_arch.h` |
| `third_party/patches/musl/0003-*.patch` | Single-thread no-op lock stubs |
| `third_party/configs/busybox_ppap_defconfig` | Minimal NOMMU config for PPAP |
| `third_party/build-musl.sh` | Patch + configure + build musl libc.a |
| `third_party/build-busybox.sh` | Patch + configure + build busybox binary |
| `src/kernel/syscall/syscall.c` | Extended dispatch table (~50 syscalls) |
| `src/kernel/syscall/sys_signal.c` | sigprocmask, sigsuspend |
| `src/kernel/syscall/sys_proc.c` | getuid/gid, setpgid, setsid, uname, etc. |
| `src/kernel/syscall/sys_io.c` | writev, ioctl, fcntl |
| `src/kernel/syscall/sys_fs.c` | access, readlink, lstat, umask, link, symlink, `*at` family |
| `src/kernel/syscall/sys_mem.c` | mmap2/munmap anonymous |
| `src/kernel/syscall/sys_time.c` | clock_gettime, gettimeofday, times |
| `src/kernel/fd/tty.c` | termios support, line discipline, ISIG/ICANON |
| `src/kernel/fs/devfs.c` | /dev/tty, /dev/console entries |
| `src/kernel/fs/procfs.c` | /proc/self, /proc/<pid>/* |
| `src/kernel/signal/signal.c` | sigprocmask integration, blocked mask |
| `src/kernel/proc/proc.h` | PCB: pgid, sid, umask, mmap regions |
| `src/kernel/exec/exec.c` | Large binary support, argv/envp passing |
| `romfs/etc/inittab` | busybox init configuration |
| `romfs/etc/fstab` | Filesystem mount table |
| `romfs/etc/passwd` | User database |
| `romfs/etc/group` | Group database |
| `romfs/etc/profile` | Shell startup script |
| `romfs/bin/busybox` | Static musl-linked busybox binary |
| `romfs/bin/*` → `busybox` | Symlink farm for all enabled applets |
| `romfs/sbin/init` → `../bin/busybox` | Init symlink |

---

## Known Risks and Mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| musl internal code uses Thumb-2 instructions (unavailable on M0+) | Medium | Build with `-march=armv6s-m` to ensure compiler emits only Thumb-1; grep musl's inline asm for non-M0+ instructions |
| busybox binary too large for romfs flash | Low | 300–400 KB fits in 16 MB flash; reduce applet set if needed |
| busybox uses `fork()` paths despite NOMMU config | Medium | Enable `CONFIG_NOMMU=y`; audit busybox source for remaining `fork()` calls; patch if needed |
| Missing syscall causes infinite crash-respawn loop | High | Enable `SYSCALL_DEBUG` during bring-up; add `--noexec` dry-run mode to init |
| musl's malloc exhausts page pool via mmap | Medium | Set `MMAP_THRESHOLD` high to force brk-only allocation; or limit mmap to 2 pages per call |
| GOT too large for single SRAM page (PIC busybox) | Medium | Use ET_EXEC (fixed-address) linking instead of PIC; or split GOT across pages in loader |
| Termios ioctl response doesn't match ash expectations | High | Test incrementally: first get ash to print prompt, then handle input, then line editing |
| UART RX overrun during fast typing | Medium | Increase RX ring buffer to 256 bytes; add XON/XOFF software flow control if needed |
| `/etc/inittab` parsing differences from Linux | Low | Test busybox init's inittab parser with exact format; keep inittab minimal |
| Signal delivery during syscall causes corruption | Medium | Ensure signal check happens only at safe points (post-syscall return, not mid-dispatch) |
| Atomic operation stubs cause subtle bugs in musl | Low | musl atomics are only used for internal locking — with no-op locks, atomics are dead code |
| Large busybox ELF overwhelms romfs-in-ROM on QEMU | Low | QEMU ROM region is 8 MB — sufficient; or reduce test romfs for QEMU |

---

## References

- [musl libc](https://musl.libc.org/) — Source, porting notes, build system documentation
- [musl wiki — Porting](https://wiki.musl-libc.org/porting.html) — Architecture porting guide
- [busybox](https://busybox.net/) — Build system, NOMMU configuration, applet list
- [busybox NOMMU docs](https://busybox.net/FAQ.html#nommu) — vfork restrictions and NOMMU applet support
- [RP2040 Datasheet](https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf) — SysTick timer for boot_ticks
- [ARMv6-M Architecture Reference Manual](https://developer.arm.com/documentation/ddi0419/) — SVC exception, CONTROL register
- [PicoPiAndPortable Design Spec v0.4](PicoPiAndPortable-spec-v04.md) — §5 (System Calls), §6 (busybox Strategy), §7 (Boot Sequence)
- [FUZIX on RP2040](https://github.com/EtchedPixels/FUZIX) — Reference for NOMMU UNIX on Cortex-M0+
