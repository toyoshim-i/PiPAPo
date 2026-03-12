# PPAP Userland Application Developer Guide

This document describes how to build, link, and deploy user-space
applications for the PiPAPo (PPAP) operating system.

## 1. Platform Overview

PPAP runs on multiple architectures. Key characteristics shared by all targets:

- **No MMU**: flat memory model, no virtual addressing
- **Single-user**: uid/gid always 0 (root)
- **NOMMU process model**: `vfork()` only, no `fork()` (no COW)
- **PIC binaries**: all user code is position-independent

### Per-Architecture Details

| | ARM (RP2040) | m68k (QEMU) |
|---|---|---|
| ISA | ARMv6-M (Thumb-1 only) | Motorola 68000 |
| FPU | None (software FP) | None (software FP) |
| XIP flash | Yes (code at 0x10000000+) | No (code in RAM) |
| PIC register | r9 (GOT base) | a5 (data segment base) |
| PIC flags | `-fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative` | `-fPIC -msep-data` |
| HW divide | No (libgcc) | Yes |
| Compiler | `arm-none-eabi-gcc` | `m68k-elf-gcc` |

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

Use raw syscall stubs. No standard C library — only freestanding
headers (`<stdint.h>`, `<stddef.h>`). This produces the smallest binaries
and is suitable for test programs and simple utilities.

Reference implementation: `src/user/` directory.

### Path B: musl libc

Link against musl libc for full POSIX C library support (`printf`,
`malloc`, `fopen`, etc.). This is what busybox uses and is the
recommended path for non-trivial applications.

Reference implementations:
- `third_party/build_busybox.sh` — busybox (multicall binary)
- `third_party/build_rogue.sh` — Rogue 5.4.4 (standalone build script, minimal curses shim)

## 3. Toolchain Requirements

### ARM Targets

- `arm-none-eabi-gcc` (version 10 or later)
- `arm-none-eabi-binutils`

On Ubuntu/Debian:
```sh
sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi
```

### m68k Targets

- `m68k-elf-gcc` — custom-built cross compiler (see `scripts/build-m68k-toolchain.sh`)
- `m68k-elf-binutils`

### musl Sysroot (Path B only)

Build the musl sysroot before compiling applications:

```sh
./third_party/build_musl.sh       # builds ARM sysroot
./third_party/build_musl.sh m68k  # builds m68k sysroot (if supported)
```

This produces `build/musl-sysroot/` (or architecture-specific variant) containing:
- `lib/libc.a` — static C library
- `lib/crt1.o`, `crti.o`, `crtn.o` — CRT startup objects
- `include/` — POSIX headers

## 4. Compiler Flags

### Mandatory Flags — ARM

Every ARM PPAP userland binary **must** be compiled with all of these flags:

```
-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft
-fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative
```

| Flag | Purpose |
|------|---------|
| `-mthumb` | Generate Thumb instructions |
| `-mcpu=cortex-m0plus` | Target the RP2040's CPU core |
| `-mfloat-abi=soft` | Software floating point |
| `-fPIC` | Position-independent code (all data via GOT) |
| `-msingle-pic-base` | Use a single register (r9) as the GOT base |
| `-mpic-register=r9` | Designate r9 as the PIC register |
| `-mno-pic-data-is-text-relative` | **Critical.** Prevents the compiler from assuming data is at a fixed offset from code. Required because `.text` lives in flash (XIP) while `.data`/`.got` live in SRAM. |

### Mandatory Flags — m68k

```
-m68000 -fPIC -msep-data
```

| Flag | Purpose |
|------|---------|
| `-m68000` | Target Motorola 68000 |
| `-fPIC` | Position-independent code |
| `-msep-data` | Separate text and data segments (a5 = data base) |

### Additional Flags (Path A: Bare-Metal)

```
-ffreestanding -nostdlib -Os -g -Wall -Werror
```

### Additional Flags (Path B: musl)

```
-Os -nostdinc
-isystem build/musl-sysroot/include
-isystem $(<compiler> -print-file-name=include)
-pie
```

## 5. Linking

### Linker Script

PPAP user binaries are linked at **address 0** and must produce exactly
**two PT_LOAD segments**:

1. **text** (R+X, flags=5): `.text` + `.rodata` — stays in flash (ARM XIP) or RAM (m68k)
2. **data** (R+W, flags=6): `.got` + `.data` + `.bss` — SRAM/RAM pages managed by the kernel

The kernel's ELF loader identifies segments by their flags:
- Segment with `PF_X` (execute) set → text
- Segment with `PF_W` (write) set → data

Architecture-specific linker scripts are in `src/user/arch/<arch>/user.ld`.

#### PIE Linker Script (Path B: musl)

musl programs built with `-pie` need additional sections. See
`third_party/patches/musl/libc_arm_m.ld` for the full ARM reference. Key additions:

- `.rel.dyn` / `.rela.dyn` section — contains relocation entries
- `.dynamic`, `.dynsym`, `.dynstr`, `.hash` sections — required by `-pie`
- **`.rodata` splitting** (ARM) — string literals and numeric constants stay in
  the text segment (flash-safe); function pointer tables move to the data segment
  where the kernel can patch relocations

### Verifying the ELF

After linking, verify the binary has the correct structure:

```sh
# Check program headers — must have exactly 2 LOAD segments
<compiler>-readelf -l myapp

# Check data segment size (must be <= 128 KB)
<compiler>-size myapp

# Check relocations (PIE binaries only)
<compiler>-readelf -r myapp
```

## 6. ELF Loader Details

The kernel ELF loader (`src/kernel/exec/exec.c`) supports both ARM and m68k ELFs:

### Format Requirements

| Field | ARM | m68k |
|-------|-----|------|
| Magic | `\x7fELF` | `\x7fELF` |
| Class | ELF32 | ELF32 |
| Data | Little-endian | Big-endian |
| Machine | `EM_ARM` (40) | `EM_68K` (4) |
| Type | `ET_EXEC` or `ET_DYN` (PIE) | `ET_EXEC` or `ET_DYN` (PIE) |

### Segment Limits

- Maximum PT_LOAD segments: 4
- Maximum data pages: 32 (128 KB)
- Stack allocation: 1 page (4 KB)

### Relocation

The loader performs architecture-specific relocation at exec time:

**ARM:** GOT patching (r9 = GOT base) + `R_ARM_RELATIVE` for PIE. Text references point to flash XIP addresses; data references point to SRAM.

**m68k:** GOT patching (a5 = data base) + `R_68K_RELATIVE` for PIE. Both text and data are in RAM.

**Important**: Section headers must be present in the ELF for the loader to
discover `.got` and relocation sections. Do not strip section headers.

### Initial Stack Layout

The kernel builds an argc/argv/auxv stack:

```
[stack top = page + 4096]
  path string: "/bin/myapp\0"
  <padding to alignment>
  auxv[1] = { AT_NULL(0),   0    }
  auxv[0] = { AT_PAGESZ(6), 4096 }
  envp[0] = NULL
  argv[1] = NULL
  argv[0] = pointer to path string
  argc    = 1                        <-- SP at entry to _start
```

## 7. User Process Memory Layout

PPAP uses a flat (no MMU) memory model. Each process has its own set of
kernel-managed pages for data, heap, and stack, but there is no hardware
address isolation between processes.

### ARM (RP2040) Layout

On ARM targets, code stays in XIP flash and is never copied to SRAM.
Data and heap share a contiguous page region in SRAM.

Note: addresses increase downward in this diagram (low addresses at top).

```
          XIP Flash (0x10000000)
┌──────────────────────────────────────┐
│  boot2 + stage1        (4 KB)        │  0x10000000  QSPI init, set VTOR
├──────────────────────────────────────┤
│  Kernel .vectors       (256 B)       │  0x10001000  Vector table (VTOR)
│  Kernel .text + .rodata (80 KB)      │              Kernel code
├──────────────────────────────────────┤
│  romfs image           (~1.9 MB)     │  0x10015000  Read-only filesystem
├──────────────────────────────────────┤
│  User .text  (execute-in-place)      │              Read + Execute
│  User .rodata                        │              (never copied to RAM)
└──────────────────────────────────────┘

          SRAM (0x20000000)
┌──────────────────────────────────────┐
│  Kernel .data + .bss     (≤16 KB)    │  0x20000000  RAM_KERNEL (20 KB)
│  Kernel stack (MSP)      (4 KB)      │              Supervisor stack
├──────────────────────────────────────┤
│  Page pool                           │  0x20005000  RAM_PAGES (204 KB)
│  ┌──────────────────────────────┐    │
│  │  .got   (Global Offset Table)│    │  r9 = GOT base
│  │  .data  (initialized data)   │    │  Copied from ELF at exec
│  │  .bss   (zero-initialized)   │    │  Zeroed at exec
│  ├──────────────────────────────┤    │
│  │  brk_base                    │    │  ← initial break (16-byte aligned)
│  │  Heap (toward higher addr)   │    │  Expanded by musl malloc / sbrk
│  │        ...                   │    │  New pages allocated on demand
│  │  brk_current                 │    │  ← current break
│  └──────────────────────────────┘    │
│  (up to USER_PAGES_MAX = 64 pages)   │
│                                      │
│  ┌──────────────────────────────┐    │
│  │  argument strings            │    │  high address
│  │  <alignment padding>         │    │
│  │  auxv[] = {AT_PAGESZ, ...}   │    │
│  │  envp[0]=NULL                │    │
│  │  argv[0], argv[1]=NULL       │    │
│  │  argc                        │    │  ← SP (PSP) at entry
│  │  ─────────────────────────── │    │
│  │  Local variables, call frames│    │  (grows toward lower addr)
│  │        ...                   │    │
│  └──────────────────────────────┘    │
│  Stack page  (4 KB, separate page)   │
├──────────────────────────────────────┤
│  I/O buffers             (24 KB)     │  0x20038000  RAM_IOBUF
├──────────────────────────────────────┤
│  DMA reserved            (16 KB)     │  0x2003E000  RAM_DMA
└──────────────────────────────────────┘  0x20042000  End of SRAM (264 KB)
```

For `pico1calc`, the same model applies but the fixed SRAM split is:

- `RAM_KERNEL`: `0x20000000`–`0x20005FFF` (24 KB)
- `RAM_PAGES`: `0x20006000`–`0x20037FFF` (200 KB, 50 pages)

This extra 4 KB of kernel SRAM leaves one fewer user page than `pico1`, but
keeps the I/O and DMA regions unchanged.

**Key points:**

- Text executes directly from flash (XIP) — zero RAM cost for code.
- GOT is patched at exec time: entries pointing into text get flash
  addresses, entries pointing into data get SRAM addresses.
- Data pages are allocated contiguously so that `brk()` can extend the
  region by appending pages at the end.
- The stack is a separate single page (4 KB), not contiguous with data.
- Stack pointer (PSP) starts at the top of the stack page, below the
  argc/argv/auxv block built by the kernel.

### m68k (PPAP Native ELF) Layout

On m68k targets running native PPAP ELF binaries (using `trap #0`
syscalls), text lives in RAM (romfs is memory-mapped, not flash XIP).
The data/heap and stack layout is similar to ARM but uses separate
kernel (SSP) and user (USP) stack pages.

Note: addresses increase downward in this diagram (low addresses at top).

```
          RAM (0x00000000, 16 MB on QEMU virt)
┌──────────────────────────────────────┐
│  Vector table            (1 KB)      │  0x00000000  256 vectors × 4 bytes
│  Kernel .text + .rodata              │  0x00000400  Kernel code
│  Kernel .data + .bss                 │              Kernel data
│  Kernel stack (SSP)      (16 KB)     │              Boot supervisor stack
├──────────────────────────────────────┤
│  Page pool               (remainder) │  __page_pool_start (4K-aligned)
│                                      │
│  User .text  (in romfs, RAM-mapped)  │              Read + Execute
│  User .rodata                        │              (no XIP — RAM only)
│                                      │
│  ┌──────────────────────────────┐    │
│  │  .got   (Global Offset Table)│    │  a5 = GOT base (PIC register)
│  │  .data  (initialized data)   │    │  Copied from ELF at exec
│  │  .bss   (zero-initialized)   │    │  Zeroed at exec
│  ├──────────────────────────────┤    │
│  │  brk_base                    │    │  ← initial break (16-byte aligned)
│  │  Heap (toward higher addr)   │    │  Expanded by musl malloc / sbrk
│  │        ...                   │    │  New pages allocated on demand
│  │  brk_current                 │    │  ← current break
│  └──────────────────────────────┘    │
│  (up to USER_PAGES_MAX pages)        │
│                                      │
│  ┌──────────────────────────────┐    │
│  │  argument strings            │    │  high address
│  │  <alignment padding>         │    │
│  │  auxv[] = {AT_PAGESZ, ...}   │    │
│  │  envp[0]=NULL                │    │
│  │  argv[0], argv[1]=NULL       │    │
│  │  argc                        │    │  ← USP at entry
│  │  ─────────────────────────── │    │
│  │  Local variables, call frames│    │  (grows toward lower addr)
│  │        ...                   │    │
│  └──────────────────────────────┘    │
│  User stack page (USP, 4 KB)         │  Separate page for user mode
│                                      │
│  Kernel stack page (SSP, per-proc)   │  Separate page for trap handling
│                                      │
│  (remaining free pages)              │
└──────────────────────────────────────┘
```

**Key points:**

- Unlike ARM, both text and data are in RAM (no flash XIP on m68k).
- GOT is patched at exec time via `a5` (PIC register), same as ARM's `r9`.
- The m68k has separate SSP (supervisor) and USP (user) stack pointers.
  The kernel allocates two stack pages: one for SSP (trap/exception
  handling) and one for USP (user execution with argc/argv).
- Data pages and heap work identically to ARM — contiguous allocation,
  `brk()` expansion, same `USER_PAGES_MAX` limit.

### m68k (Human68k Subsystem) Layout

For Human68k X-format (.x) binaries running under the X68k subsystem,
the memory layout differs: a single contiguous block with a 256-byte
PMB header, text, data, BSS, and shared heap/stack space. See
[subsystems/human68k.md §7.2](/docs/subsystems/human68k.md#72-ppap-memory-layout-for-human68k-processes)
for the full layout diagram, PMB field map, initial register values,
and details on `_SETBLOCK` / `_MALLOC` heap management.

### Memory Limits

| Resource | Limit | Notes |
|----------|-------|-------|
| Data + heap pages | 64 pages (256 KB) | `USER_PAGES_MAX`; m68k targets may override to 512 pages (2 MB) |
| Stack | 1 page (4 KB) | ARM: user PSP; m68k: kernel SSP (user stack is within the data block) |
| Page size | 4096 bytes | All architectures |
| Heap alignment | 16-byte | `brk_base` aligned up to 16 bytes (musl malloc requirement) |

### Heap Management

**ARM (ELF):** The heap is managed via the `brk` syscall.

- `brk(0)` — query current break without changing it
- `brk(addr)` — set the break to `addr`; the kernel allocates or frees
  pages as needed. Returns the resulting break (unchanged on failure).
- musl's `malloc` uses `brk` internally. Bare-metal programs can call
  `brk` directly or implement their own allocator on top of it.

**m68k (X-format):** Heap is managed via Human68k DOS calls (`_MALLOC`,
`_MFREE`, `_SETBLOCK`). See
[subsystems/human68k.md §7.4](/docs/subsystems/human68k.md#74-memory-management-calls).

### mmap

Anonymous `mmap` is supported for additional allocations beyond `brk`:

- `MAP_ANONYMOUS | MAP_PRIVATE` — allocates zero-filled pages
- `MAP_FIXED` — allocates at a specific address
- Up to 8 mmap regions per process
- File-backed mmap is not supported

## 8. Syscall Interface

PPAP uses a **unified 16-bit grouped numbering** scheme shared across all
architectures. The trap mechanism is architecture-specific:

### ARM Convention

| Register | Purpose |
|----------|---------|
| r7 | Syscall number |
| r0-r3 | Arguments 1-4 |
| r4-r5 | Arguments 5-6 |
| `svc 0` | Trigger the syscall |
| r0 | Return value (negative errno on error) |

### m68k Convention

| Register | Purpose |
|----------|---------|
| d0 | Syscall number → return value |
| d1-d5 | Arguments 1-5 |
| a0 | Argument 6 |
| `trap #0` | Trigger the syscall |

See [syscall.md](/docs/kernel/syscall.md) for the complete syscall reference.

## 9. Path A: Bare-Metal Development

### Directory Structure

```
src/user/
  arch/arm_m/   ARM-specific: crt0.S, syscall.S, user.ld
  arch/m68k/    m68k-specific: crt0.S, syscall.S, user.ld
  syscall.h     C declarations for the stubs
  hello.c       Example: "Hello from user space!"
```

Build rules are in `cmake/user.cmake` — user programs are compiled as
custom commands via `ppap_user_program()` and linked into the romfs image.

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

The `crt0.S` entry point (architecture-specific) calls `main(0, NULL)` and
then issues `_exit()` with main's return value.

### Adding a New Program

1. Create `src/user/myapp.c`
2. Add `myapp` to the `USER_APPS` list in `cmake/user.cmake`
3. Build: CMake links `crt0.o + syscall.o + myapp.o → myapp.elf`

## 10. Path B: musl-Based Development

### Prerequisites

```sh
# Build musl sysroot (one-time, per architecture)
./third_party/build_musl.sh
```

### Build Process

1. **Generate specs file** (see section 5 or reference build scripts)
2. **Write or copy a linker script** — start from `src/user/arch/<arch>/user.ld`
   for simple programs, or use `third_party/patches/musl/libc_arm_m.ld` for PIE
3. **Compile and link** with the appropriate architecture flags

### When to Use `-pie` and `.rodata` Splitting (ARM)

**Simple programs** (no function pointer arrays in const data): use the
basic linker script without `-pie`. All `.rodata` stays in the text segment.

**Complex programs** (function pointer dispatch tables): use `-pie` and a
linker script that splits `.rodata`:
- `.rodata.str*` (string literals) → text segment (flash-safe, no addresses)
- `.rodata.cst*` (numeric constants) → text segment (flash-safe)
- `.rodata` (everything else) → data segment (kernel patches relocations)

On m68k, this splitting is not needed because both text and data are in RAM.

## 11. Packaging and Deployment

### romfs Image

User binaries are packaged into a romfs image that is linked into the
kernel binary. The romfs filesystem is read-only.

#### Directory Structure

The romfs image is assembled at build time from two sources:

- **`src/etc/`** — config file templates (fstab, passwd, inittab, …)
- **`build/<arch>/romfs/`** — built binaries (busybox, rogue, user programs)

Per-target staging merges these into `build/<arch>/romfs_<target>/`.

#### mkromfs Tool

The `tools/mkromfs/mkromfs.c` tool creates romfs images:

```sh
build/<arch>/mkromfs build/<arch>/romfs_<target>/ build/<arch>/romfs.bin
```

### Writable Filesystems

At runtime, these writable filesystems are available:

| Mount Point | Type | Purpose |
|-------------|------|---------|
| `/dev` | devfs | Device nodes (null, zero, ttyS0, console, urandom) |
| `/proc` | procfs | Process info (meminfo, version) |
| `/tmp` | tmpfs | RAM-backed temporary storage |
| `/mnt/sd` | vfat | SD card (if present, FAT32) |

## 12. Testing

### QEMU

PPAP includes QEMU targets for both architectures:

```sh
# ARM
./scripts/run.sh --build qemu_arm

# m68k
./scripts/run.sh --build qemu_m68k

# With tests
./scripts/run.sh --test qemu_arm
```

The kernel runs integration tests at boot, then launches `/sbin/init`
(or `/bin/runtests` in test mode).

### Hardware (ARM / RP2040)

```sh
./scripts/run.sh --build pico1calc
```

Connect a serial terminal to the UART (115200 baud, 8N1).

## 13. Porting Third-Party Applications

Existing UNIX applications can be ported to PPAP if they fit within the
per-process memory budget (128 KB data+bss). The recommended pattern:

1. **Import** the upstream source as a git submodule under `third_party/`
2. **Create patches** under `third_party/patches/<app>/` — PPAP-specific
   headers injected via `-isystem`
3. **Write a build script** `third_party/build-<app>.sh` that cross-compiles
   against musl
4. **Integrate with CMake** — add a custom command and wire into the romfs
   dependency chain
5. **Build for each target architecture** as needed

See [porting.md](/docs/getting_started/porting.md) for the detailed guide.

## 14. Known Limitations

- **No shared libraries**: all linking is static (`libc.a`)
- **No `fork()`**: only `vfork()` is available (NOMMU model). The child
  must call `_exit()` or `execve()` before the parent resumes.
- **No MMU**: `mprotect()` is a no-op, memory regions are not fully isolated
- **No FPU**: all floating point is software-emulated (all targets)
- **No threads**: `clone()` only supports `SIGCHLD+0` (equivalent to vfork).
  `pthread_create()` will fail.
- **128 KB data limit**: the data segment must fit in 32 pages
- **4 KB stack**: deep recursion or large stack allocations will overflow
- **Single-user**: all uid/gid syscalls return 0
- **No RTC**: time starts at 0 on boot, incremented by timer
- **Read-only root**: romfs is in flash/ROM; use `/tmp` or `/mnt/sd` for
  writable storage
- **Section headers required**: do not strip section headers from ELF
  binaries (the loader needs them to find `.got` and relocation sections)
