# PPAP Userland Application Developer Guide

This document describes how to build, link, and deploy user-space
applications for the PicoPiAndPortable (PPAP) operating system.

## 1. Platform Overview

PPAP runs on the RP2040 microcontroller (dual Cortex-M0+, 264 KB SRAM,
2 MB flash). Key characteristics:

- **ISA**: ARMv6-M (Thumb-1 only, no Thumb-2 IT blocks or 32-bit multiply)
- **No MMU**: flat memory model, no virtual addressing
- **No FPU**: software floating point only
- **XIP flash**: code executes directly from flash at 0x10000000+
- **Single-user**: uid/gid always 0 (root)
- **NOMMU process model**: `vfork()` only, no `fork()` (no COW)

### Memory Layout

| Region | Address Range | Size | Purpose |
|--------|---------------|------|---------|
| Kernel | 0x20000000-0x20004FFF | 20 KB | Kernel BSS, data, stack |
| Page pool | 0x20005000-0x20037FFF | 204 KB | User pages (51 x 4 KB) |
| I/O buffer | 0x20038000-0x2003DFFF | 24 KB | UART and filesystem buffers |
| DMA | 0x2003E000-0x2003FFFF | 8 KB | DMA / Core 1 |

### Per-Process Limits

| Resource | Limit |
|----------|-------|
| Data segment (GOT + .data + .bss + .rodata) | 32 pages = 128 KB |
| Stack | 1 page = 4 KB |
| File descriptors | 16 |
| Concurrent processes | 8 (system-wide) |
| mmap regions | 4 per process |

## 2. Development Paths

There are two ways to write applications for PPAP:

### Path A: Bare-Metal (no libc)

Use raw SVC syscall stubs. No standard C library — only freestanding
headers (`<stdint.h>`, `<stddef.h>`). This produces the smallest binaries
and is suitable for test programs and simple utilities.

Reference implementation: `user/` directory.

### Path B: musl libc

Link against musl libc for full POSIX C library support (`printf`,
`malloc`, `fopen`, etc.). This is what busybox uses and is the
recommended path for non-trivial applications.

Reference implementations:
- `third_party/build-busybox.sh` — busybox (multicall binary, custom Makefile integration)
- `third_party/build-rogue.sh` — Rogue 5.4.4 (standalone build script, minimal curses shim)

## 3. Toolchain Requirements

### Required Tools

- `arm-none-eabi-gcc` (version 10 or later)
- `arm-none-eabi-binutils` (ld, objdump, readelf, size, nm)
- GNU make

On Ubuntu/Debian:
```sh
sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi
```

### musl Sysroot (Path B only)

Build the musl sysroot before compiling applications:

```sh
./third_party/build-musl.sh
```

This produces `build/musl-sysroot/` containing:
- `lib/libc.a` — static C library
- `lib/crt1.o`, `crti.o`, `crtn.o` — CRT startup objects
- `include/` — POSIX headers

## 4. Compiler Flags

### Mandatory Flags (both paths)

Every PPAP userland binary **must** be compiled with all of these flags:

```
-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft
-fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative
```

| Flag | Purpose |
|------|---------|
| `-mthumb` | Generate Thumb instructions (required by Cortex-M0+) |
| `-mcpu=cortex-m0plus` | Target the RP2040's CPU core |
| `-march=armv6s-m` | ARMv6-M architecture (Thumb-1 subset) |
| `-mfloat-abi=soft` | Software floating point (no FPU on RP2040) |
| `-fPIC` | Position-independent code (all data via GOT) |
| `-msingle-pic-base` | Use a single register (r9) as the GOT base |
| `-mpic-register=r9` | Designate r9 as the PIC register |
| `-mno-pic-data-is-text-relative` | **Critical.** Prevents the compiler from assuming data is at a fixed offset from code. Required because `.text` lives in flash (XIP) while `.data`/`.got` live in SRAM at unrelated addresses. |

### Additional Flags (Path A: Bare-Metal)

```
-ffreestanding -nostdlib -Os -g -Wall -Werror
```

### Additional Flags (Path B: musl)

```
-Os -nostdinc
-isystem build/musl-sysroot/include
-isystem $(arm-none-eabi-gcc -print-file-name=include)
-pie
```

| Flag | Purpose |
|------|---------|
| `-nostdinc` | Exclude default system headers (use musl's instead) |
| `-isystem .../musl/include` | Use musl headers (listed first to win `stdint.h`) |
| `-isystem .../gcc/include` | Fall back to GCC builtins (`stdarg.h`, etc.) |
| `-pie` | Generate a position-independent executable. Produces `R_ARM_RELATIVE` relocations in `.rel.dyn` so the kernel can fix up function-pointer tables in the data segment. |

## 5. Linking

### GCC Specs File (Path B: musl)

A specs file overrides GCC's default CRT objects and libraries to use musl
instead of newlib. Generate it like this:

```sh
MUSL_SYSROOT=build/musl-sysroot
GCC_LIBDIR=$(dirname "$(arm-none-eabi-gcc -mthumb -mcpu=cortex-m0plus -print-libgcc-file-name)")

cat > build/musl-arm.specs <<SPECS
*startfile:
$MUSL_SYSROOT/lib/crt1.o $MUSL_SYSROOT/lib/crti.o

*endfile:
$MUSL_SYSROOT/lib/crtn.o

*lib:
$MUSL_SYSROOT/lib/libc.a

*libgcc:
$GCC_LIBDIR/libgcc.a
SPECS
```

Use it with: `-specs=build/musl-arm.specs`

### Linker Script

PPAP user binaries are linked at **address 0** and must produce exactly
**two PT_LOAD segments**:

1. **text** (R+X, flags=5): `.text` + `.rodata` — stays in flash, executed via XIP
2. **data** (R+W, flags=6): `.got` + `.data` + `.bss` — copied to SRAM by the kernel

The kernel's ELF loader identifies segments by their flags:
- Segment with `PF_X` (execute) set → text (flash)
- Segment with `PF_W` (write) set → data (SRAM)

#### Simple Linker Script (Path A)

The `user/user.ld` script is sufficient for bare-metal programs:

```ld
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS
{
    . = 0;

    .text : { *(.text.crt0) *(.text*) } :text
    .rodata : { *(.rodata*) } :text

    . = ALIGN(4);
    .got  : { *(.got*) } :data
    .data : { *(.data*) } :data
    .bss  : {
        __bss_start = .;
        *(.bss*) *(COMMON)
        __bss_end = .;
    } :data

    _end = .;

    /DISCARD/ : { *(.ARM.attributes) *(.ARM.exidx*) *(.comment) *(.note*) }
}
```

#### PIE Linker Script (Path B: musl)

musl programs built with `-pie` need additional sections. See
`third_party/configs/busybox.ld` for the full reference. Key additions:

- `.rel.dyn` section (bounded by `__rel_dyn_start`/`__rel_dyn_end`) —
  contains `R_ARM_RELATIVE` relocation entries
- `.dynamic`, `.dynsym`, `.dynstr`, `.hash` sections — required by `-pie`
- `.init` / `.fini` sections with `KEEP()` — musl CRT init/fini code
- **`.rodata` splitting** — string literals (`.rodata.str*`) and numeric
  constants (`.rodata.cst*`) stay in the text segment (flash-safe, no
  relocatable addresses). All other `.rodata` (function pointer tables,
  dispatch arrays) moves to the data segment where the kernel can patch
  relocation entries:

```ld
/* In text segment — safe, contains no addresses */
.rodata.str : { *(.rodata.str*) } :text
.rodata.cst : { *(.rodata.cst*) } :text

/* In data segment — may contain function pointers needing relocation */
.rodata : { *(.rodata*) } :data
```

### Link Commands

**Path A (bare-metal):**
```sh
arm-none-eabi-gcc -nostdlib -T user/user.ld -Wl,--gc-sections \
    crt0.o syscall.o myapp.o -o myapp.elf
```

**Path B (musl):**
```sh
arm-none-eabi-gcc \
    -mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft \
    -fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative \
    -pie -specs=build/musl-arm.specs -T myapp.ld \
    myapp.o -o myapp
```

### Verifying the ELF

After linking, verify the binary has the correct structure:

```sh
# Check program headers — must have exactly 2 LOAD segments
arm-none-eabi-readelf -l myapp

# Check data segment size (must be <= 128 KB)
arm-none-eabi-size myapp

# Check relocations (PIE binaries only)
arm-none-eabi-readelf -r myapp
```

Expected `readelf -l` output:
```
Program Headers:
  Type   Offset   VirtAddr   PhysAddr   FileSiz MemSiz  Flg Align
  LOAD   0x...    0x00000000 0x00000000 0x...   0x...   R E 0x...
  LOAD   0x...    0x000..... 0x000..... 0x...   0x...   RW  0x...
```

## 6. ELF Loader Details

The kernel ELF loader (`src/kernel/exec/exec.c`) imposes these constraints:

### Format Requirements

| Field | Required Value |
|-------|---------------|
| Magic | `\x7fELF` |
| Class | ELF32 (32-bit) |
| Data | Little-endian |
| Machine | ARM (`EM_ARM = 40`) |
| Type | `ET_EXEC` (2) or `ET_DYN` (3, for PIE) |
| EABI | Version 5 (`e_flags & 0xFF000000 == 0x05000000`) |

### Segment Limits

- Maximum PT_LOAD segments: 4
- Maximum data pages: 32 (128 KB)
- Stack allocation: 1 page (4 KB)

### Relocation

The loader performs two kinds of relocation at exec time:

1. **GOT patching**: each entry in `.got` is classified as a text reference
   (offset < data segment VMA → points to flash) or data reference
   (offset >= data segment VMA → points to SRAM). The kernel rewrites
   each entry with the correct runtime address. Register r9 is set to the
   GOT base address in SRAM.

2. **R_ARM_RELATIVE** (PIE only): entries in `.rel.dyn` that fall within
   the data segment are patched using the same text/data classification.
   Text-segment entries are skipped (flash is read-only via XIP).

**Important**: Section headers must be present in the ELF for the loader to
discover `.got` and `.rel.dyn`. Do not strip section headers.

### Initial Stack Layout

The kernel builds an argc/argv/auxv stack that musl's `_start` expects:

```
[stack top = page + 4096]
  path string: "/bin/myapp\0"
  <padding to 8-byte alignment>
  auxv[1] = { AT_NULL(0),   0    }
  auxv[0] = { AT_PAGESZ(6), 4096 }
  envp[0] = NULL
  argv[1] = NULL
  argv[0] = pointer to path string
  argc    = 1                        <-- SP at entry to _start
```

For bare-metal programs using `crt0.S`, the stack is set to the top of the
page and argc/argv are not used.

## 7. Syscall Interface

### Convention

Syscalls use the ARM EABI convention:

| Register | Purpose |
|----------|---------|
| r7 | Syscall number |
| r0-r3 | Arguments 1-4 |
| r4-r5 | Arguments 5-6 (for 6-arg syscalls like mmap2) |
| `svc 0` | Trigger the syscall |
| r0 | Return value (negative errno on error) |

### Bare-Metal Syscall Stub Pattern

```asm
.thumb_func
my_syscall:
    push {r7, lr}
    movs r7, #NR        @ syscall number
    svc  0
    pop  {r7, pc}
```

### Implemented Syscalls

#### Process Management

| Nr | Name | Signature | Notes |
|----|------|-----------|-------|
| 1 | exit | `void _exit(int status)` | Also handles exit_group (248) |
| 2 | fork | `pid_t fork(void)` | Routes to vfork (NOMMU) |
| 11 | execve | `int execve(const char *path, ...)` | Only path arg used |
| 20 | getpid | `pid_t getpid(void)` | |
| 64 | getppid | `pid_t getppid(void)` | |
| 7 | waitpid | `pid_t waitpid(pid, *status, opts)` | |
| 114 | wait4 | `pid_t wait4(pid, *status, opts, *ru)` | rusage ignored |
| 120 | clone | `pid_t clone(flags, stack)` | Only SIGCHLD+0 → vfork |
| 190 | vfork | `pid_t vfork(void)` | Parent blocks until child exits/execs |
| 256 | set_tid_address | `long set_tid_address(int *tidptr)` | Returns current pid |

#### File I/O

| Nr | Name | Signature | Notes |
|----|------|-----------|-------|
| 3 | read | `ssize_t read(fd, buf, count)` | |
| 4 | write | `ssize_t write(fd, buf, count)` | |
| 5 | open | `int open(path, flags, mode)` | |
| 6 | close | `int close(fd)` | |
| 19 | lseek | `off_t lseek(fd, offset, whence)` | |
| 140 | _llseek | `int _llseek(fd, hi, lo, *result, whence)` | 64-bit seek |
| 41 | dup | `int dup(oldfd)` | |
| 63 | dup2 | `int dup2(oldfd, newfd)` | |
| 42 | pipe | `int pipe(int fd[2])` | |
| 145 | readv | `ssize_t readv(fd, iov, iovcnt)` | |
| 146 | writev | `ssize_t writev(fd, iov, iovcnt)` | |
| 54 | ioctl | `int ioctl(fd, request, arg)` | TCGETS/TCSETS for TTY |
| 221 | fcntl64 | `int fcntl64(fd, cmd, arg)` | F_GETFD, F_SETFD, F_GETFL, F_SETFL |

#### Filesystem

| Nr | Name | Signature | Notes |
|----|------|-----------|-------|
| 106 | stat | `int stat(path, *buf)` | Old-style stat |
| 108 | fstat | `int fstat(fd, *buf)` | Old-style fstat |
| 195 | stat64 | `int stat64(path, *buf)` | |
| 197 | fstat64 | `int fstat64(fd, *buf)` | |
| 196 | lstat64 | `int lstat64(path, *buf)` | Follows symlinks |
| 327 | fstatat64 | `int fstatat64(dirfd, path, *buf, flags)` | AT_FDCWD only |
| 141 | getdents | `int getdents(fd, *dirp, count)` | |
| 217 | getdents64 | `int getdents64(fd, *dirp, count)` | |
| 12 | chdir | `int chdir(path)` | |
| 183 | getcwd | `int getcwd(buf, size)` | |
| 39 | mkdir | `int mkdir(path, mode)` | |
| 40 | rmdir | `int rmdir(path)` | |
| 10 | unlink | `int unlink(path)` | |
| 33 | access | `int access(path, mode)` | |
| 85 | readlink | `ssize_t readlink(path, buf, bufsiz)` | |

#### Memory Management

| Nr | Name | Signature | Notes |
|----|------|-----------|-------|
| 45 | brk | `void *brk(addr)` | addr=0 queries current break |
| 192 | mmap2 | `void *mmap2(addr, len, prot, flags, fd, pgoff)` | Anonymous only (MAP_ANONYMOUS) |
| 91 | munmap | `int munmap(addr, length)` | |
| 125 | mprotect | `int mprotect(addr, len, prot)` | Stub: returns 0 (no MMU) |
| 163 | mremap | `void *mremap(...)` | Returns ENOMEM (forces malloc fallback) |

#### Signals

| Nr | Name | Signature | Notes |
|----|------|-----------|-------|
| 37 | kill | `int kill(pid, sig)` | |
| 67 | sigaction | `int sigaction(sig, *act, *oact)` | Simplified interface |
| 119 | sigreturn | `int sigreturn(void)` | |
| 174 | rt_sigaction | `int rt_sigaction(sig, *act, *oact, sigsetsize)` | Full Linux interface |
| 175 | rt_sigprocmask | `int rt_sigprocmask(how, *set, *oset, sigsetsize)` | |
| 173 | rt_sigreturn | `int rt_sigreturn(void)` | |

#### Time

| Nr | Name | Signature | Notes |
|----|------|-----------|-------|
| 162 | nanosleep | `int nanosleep(*req, *rem)` | |
| 78 | gettimeofday | `int gettimeofday(*tv, *tz)` | SysTick-based, no RTC |
| 263 | clock_gettime | `int clock_gettime(clk, *tp)` | 32-bit timespec |
| 403 | clock_gettime64 | `int clock_gettime64(clk, *tp)` | 64-bit timespec |
| 265 | clock_nanosleep | `int clock_nanosleep(clk, flags, *req, *rem)` | 32-bit |
| 407 | clock_nanosleep64 | `int clock_nanosleep64(clk, flags, *req, *rem)` | 64-bit |

#### Session / Identity

| Nr | Name | Signature | Notes |
|----|------|-----------|-------|
| 57 | setpgid | `int setpgid(pid, pgid)` | |
| 66 | setsid | `pid_t setsid(void)` | |
| 60 | umask | `mode_t umask(mask)` | |
| 122 | uname | `int uname(*buf)` | Returns "PPAP" / "armv6m" |
| 199 | getuid32 | `uid_t getuid(void)` | Always returns 0 |
| 201 | geteuid32 | `uid_t geteuid(void)` | Always returns 0 |
| 200 | getgid32 | `gid_t getgid(void)` | Always returns 0 |
| 202 | getegid32 | `gid_t getegid(void)` | Always returns 0 |
| 240 | futex | `int futex(...)` | Stub: returns 0 (single-threaded) |

#### Stubs (return error)

| Nr | Name | Returns | Reason |
|----|------|---------|--------|
| 14 | mknod | EPERM | No device creation |
| 38 | rename | ENOSYS | Not yet implemented |
| 397 | statx | ENOSYS | Use stat64 instead |

## 8. Path A: Bare-Metal Development

### Directory Structure

```
user/
  crt0.S        Minimal CRT: _start → main() → _exit()
  syscall.S     SVC syscall stubs (read, write, open, etc.)
  syscall.h     C declarations for the stubs
  user.ld       Linker script (two-segment PIC layout)
  Makefile      Build rules
  hello.c       Example: "Hello from user space!"
```

### Example: hello.c

```c
#include "syscall.h"

int main(void)
{
    static const char msg[] = "Hello from user space!\n";
    write(1, msg, sizeof(msg) - 1);
    return 0;
}
```

The `crt0.S` entry point calls `main(0, NULL)` and then issues `_exit()`
with main's return value.

### Adding a New Program

1. Create `user/myapp.c`
2. Add `myapp` to the `PROGRAMS` list in `user/Makefile`:
   ```make
   PROGRAMS := hello myapp ...
   ```
3. Build: `make -C user`
4. The Makefile links `crt0.o + syscall.o + myapp.o → myapp.elf`

### Build Commands

```sh
# Compile
arm-none-eabi-gcc -mthumb -mcpu=cortex-m0plus -march=armv6s-m \
    -ffreestanding -nostdlib -Os -g -Wall -Werror \
    -fPIC -msingle-pic-base -mpic-register=r9 \
    -mno-pic-data-is-text-relative \
    -c -o myapp.o myapp.c

# Link
arm-none-eabi-gcc -nostdlib -T user/user.ld -Wl,--gc-sections \
    crt0.o syscall.o myapp.o -o myapp.elf
```

## 9. Path B: musl-Based Development

### Prerequisites

```sh
# Build musl sysroot (one-time)
./third_party/build-musl.sh

# Verify
ls build/musl-sysroot/lib/libc.a
```

### Build Process

1. **Generate specs file** (see section 5)
2. **Write or copy a linker script** — start from `user/user.ld` for simple
   programs, or copy `third_party/configs/busybox.ld` for PIE binaries with
   function-pointer tables in `.rodata`
3. **Compile and link**:

```sh
arm-none-eabi-gcc \
    -mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft -Os \
    -nostdinc \
    -isystem build/musl-sysroot/include \
    -isystem "$(arm-none-eabi-gcc -print-file-name=include)" \
    -fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative \
    -pie -specs=build/musl-arm.specs -T myapp.ld \
    myapp.c -o myapp
```

### Example: Minimal musl Program

```c
#include <stdio.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    printf("Hello from %s (pid %d)\n", argv[0], getpid());
    return 0;
}
```

musl's `_start` (from `crt1.o`) handles argc/argv parsing from the stack
and calls `main(argc, argv, envp)`. The kernel sets up argc=1 and
argv[0]=path at exec time.

### When to Use `-pie` and `.rodata` Splitting

**Simple programs** (no function pointer arrays in const data): use
`user/user.ld` without `-pie`. All `.rodata` stays in the text segment
(flash).

**Complex programs** (function pointer dispatch tables, vtable-like
arrays): use `-pie` and a linker script that splits `.rodata`:
- `.rodata.str*` (string literals) → text segment (no addresses, flash-safe)
- `.rodata.cst*` (numeric constants) → text segment (flash-safe)
- `.rodata` (everything else) → data segment (kernel patches relocations)

This is necessary because function pointer arrays contain absolute addresses
that the kernel must relocate at load time, and flash is read-only.

### musl Compatibility Notes

musl was built with the same PIC flags as user programs. The PPAP-specific
build applies these patches to upstream musl:

- ARM assembly files replaced with generic C fallbacks (Thumb-1 compatible)
- No shared library support (`--disable-shared`)
- No dynamic linker (ldso files removed)
- setjmp/longjmp, memcpy, vfork, clone all use C implementations

## 10. Packaging and Deployment

### romfs Image

User binaries are packaged into a romfs image that is linked into the
kernel binary. The romfs filesystem is read-only and lives in flash.

#### Directory Structure

```
romfs/
  bin/          User binaries (hello, rogue, busybox applet symlinks)
  sbin/         System binaries (init → busybox)
  etc/
    inittab     busybox init configuration
    profile     Shell startup (PATH, PS1)
    hostname    System hostname
    passwd      User database (root only)
    group       Group database
    fstab       Filesystem mount table
    motd        Message of the day
  mnt/
    sd/         SD card mount point (FAT32, runtime)
```

#### Adding Files to romfs

1. Place your binary in `romfs/bin/`:
   ```sh
   cp myapp.elf romfs/bin/myapp
   ```
2. Rebuild: `cd build && ninja`

The CMake pipeline automatically:
1. Builds user binaries (`user/Makefile`)
2. Builds busybox and installs applet symlinks
3. Builds rogue and installs to `romfs/bin/rogue`
4. Runs `mkromfs romfs/ build/romfs.bin`
5. Links `romfs.bin` into the kernel via `.incbin`

#### mkromfs Tool

The `tools/mkromfs/mkromfs.c` tool creates romfs images:

```sh
# Build the tool (host compiler, not cross-compiler)
cc -O2 -I src/kernel/fs -o build/mkromfs tools/mkromfs/mkromfs.c

# Create image
build/mkromfs romfs/ build/romfs.bin

# Inspect image contents
build/mkromfs --dump build/romfs.bin
```

Limits: max 512 entries, max 2 MB image size.

### Writable Filesystems

At runtime, these writable filesystems are available:

| Mount Point | Type | Purpose |
|-------------|------|---------|
| `/dev` | devfs | Device nodes (null, zero, ttyS0, console, urandom) |
| `/proc` | procfs | Process info (meminfo, version) |
| `/tmp` | tmpfs | RAM-backed temporary storage |
| `/mnt/sd` | vfat | SD card (if present, FAT32) |

## 11. Testing

### QEMU

PPAP includes a QEMU target (`ppap_qemu`) that runs on the `mps2-an500`
machine (Cortex-M3, compatible with M0+ Thumb-1 code):

```sh
cd build && ninja ppap_qemu

qemu-system-arm -M mps2-an500 -nographic \
    -kernel ppap_qemu.elf \
    -device loader,file=fat_test.img,addr=0x21000000 \
    -semihosting
```

The kernel runs all integration tests at boot, then launches `/sbin/init`
which starts an interactive shell.

### Hardware (RP2040)

```sh
cd build && ninja ppap

# Flash via OpenOCD + GDB
gdb-multiarch -x ppap.gdb build/ppap.elf
```

Connect a serial terminal to the UART (115200 baud, 8N1) to interact
with the shell.

## 12. Porting Third-Party Applications

Existing UNIX applications can be ported to PPAP if they fit within the
per-process memory budget (128 KB data+bss) and use only Thumb-1 compatible
code. The recommended pattern:

1. **Import** the upstream source as a git submodule under `third_party/`
2. **Create patches** under `third_party/patches/<app>/` — PPAP-specific
   headers injected via `-isystem` (avoids modifying upstream source)
3. **Write a build script** `third_party/build-<app>.sh` that cross-compiles
   against musl using the same CFLAGS/linker script as busybox
4. **Integrate with CMake** — add a custom command in `CMakeLists.txt` and
   wire into the romfs dependency chain

**Example: Rogue 5.4.4** — see `docs/history/port-rogue.md` and `third_party/build-rogue.sh`.
The port required a minimal curses shim (~800 lines) translating curses calls
to VT100 escape sequences, plus `config.h` and `pwd.h` stubs. The upstream
source is completely unmodified.

## 13. Known Limitations

- **No shared libraries**: all linking is static (`libc.a`)
- **No `fork()`**: only `vfork()` is available (NOMMU model). The child
  must call `_exit()` or `execve()` before the parent resumes.
- **No MMU**: `mprotect()` is a no-op, memory regions are not isolated
  between processes
- **No FPU**: all floating point is software-emulated
- **No threads**: `clone()` only supports `SIGCHLD+0` (equivalent to vfork).
  `pthread_create()` will fail. `futex()` returns 0 (locks are no-ops).
- **128 KB data limit**: the data segment (GOT + .rodata + .data + .bss)
  must fit in 32 pages
- **4 KB stack**: deep recursion or large stack allocations will overflow
- **Single-user**: all uid/gid syscalls return 0
- **No RTC**: time starts at 0 on boot, incremented by SysTick
- **Read-only root**: romfs is in flash; use `/tmp` or `/mnt/sd` for
  writable storage
- **Section headers required**: do not strip section headers from ELF
  binaries (the loader needs them to find `.got` and `.rel.dyn`)
