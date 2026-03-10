# PiPAPo (PPAP)

A portable UNIX-like micro OS for bare-metal microcontrollers and retro CPUs.

> Full design specification: [docs/spec-v07.md](docs/spec-v07.md)

---

## Goals

- POSIX-subset system call interface — same syscall numbers across all architectures
- Run **busybox** (statically linked) with an interactive `hush` shell
- Run **Rogue 5.4.4** (classic dungeon crawler) via a minimal VT100 curses shim
- Root file system on flash/ROM as **romfs**; SD card as **VFAT (FAT32)** with **UFS** loopback images
- Multiple target architectures and boards from a shared kernel codebase
- **PicoCalc standalone**: embedded LCD console (40×20 / 80×40), I2C keyboard — no host PC required

## Supported Targets

| Target | Board / Emulator | Architecture | CPU | RAM |
|---|---|---|---|---|
| `qemu_arm` | QEMU mps2-an500 | ARM Cortex-M0+ | ARMv6-M (Thumb-1) | 4 MB |
| `pico1` | Raspberry Pi Pico | ARM Cortex-M0+ | Dual-core @ 133 MHz | 264 KB |
| `pico1calc` | ClockworkPi PicoCalc | ARM Cortex-M0+ | Dual-core @ 133 MHz | 264 KB |
| `qemu_m68k` | QEMU virt m68k | Motorola 68000 | m68000 | Up to 16 MB (auto-detected) |

All targets share the same kernel source, syscall interface, VFS, and process model. Only drivers, boot sequences, linker scripts, and architecture-specific code (context switch, syscall trap) differ per target.

## Features

- **Kernel** — preemptive scheduler, vfork/exec, signals, pipes, memory protection
- **File systems** — romfs, VFAT (SD card), UFS (loopback images), devfs, procfs, tmpfs
- **User space** — musl libc, busybox (hush shell + 100+ applets), Rogue 5.4.4
- **Multi-architecture** — ARM (Thumb-1) and m68k from the same source tree
- **PicoCalc display** — SPI LCD framebuffer console (40×20 / 80×40), VT100/ANSI color emulator
- **PicoCalc keyboard** — I2C STM32 co-processor, full keymap with function keys
- **Multi-TTY** — serial console + LCD console with getty login on each
- **PIE/PIC binaries** — position-independent ELFs; on ARM targets, code runs from flash via XIP

## Known Issues

- **LCD TTY unstable** — the framebuffer console occasionally hangs or glitches during heavy scrolling
- **SD card disabled** — SD/VFAT support is tentatively disabled in the current build

## Future Work

- **RP2350 Port** — Cortex-M33, 8-region MPU, PSRAM support, Thumb-2 optimization; `pico2`/`pico2calc` targets
- **Pi Zero Port** — ARM1176JZF-S with full MMU, SD card boot; see [docs/target-pizero.md](docs/target-pizero.md)
- **CPU emulation** — user-space interpretive emulators for retro CPUs (Z80, 6502, 6809, 8086), enabling cross-architecture binary execution; see [docs/feature-eCPU.md](docs/feature-eCPU.md)
- **Subsystem support** — load and run applications from other OSes on top of PPAP via syscall bridge (e.g. CP/M, Human68K, DOS); see [docs/feature-subsystem.md](docs/feature-subsystem.md)
- Audio driver support

## Repository Layout

```
PPAP/
  cmake/                    Shared cmake modules (arm_m, m68k, kernel, user)
  src/
    target/
      target.h              Target abstraction API (5-function interface)
      qemu_arm/             QEMU mps2-an500 (ARM): CMSDK UART, RAM block device
        qemu.ld             ARM QEMU layout: ROM @ 0x0, RAM @ 0x20000000
      qemu_m68k/            QEMU MCF5208 (m68k): UART, RAM block device
      pico1/                Official Raspberry Pi Pico: romfs-only, no SD
        pico1.ld            Pico: 2 MB flash, 80 KB kernel @ 0x10001000
      pico1calc/            ClockworkPi PicoCalc: SPI SD card, 16 MB flash
        pico1calc.ld        PicoCalc: 16 MB flash, 96 KB kernel @ 0x10004000
    boot/
      stage1.S              Stage 1 bootloader (ARM/RP2040: sets VTOR, jumps to kernel)
    kernel/
      main.c                Unified kmain() — uses target hooks for all platforms
      mm/                   Memory management (page allocator, kmem)
      proc/                 Process management (PCB, scheduler, context switch)
      syscall/              System call layer (trap handler, dispatch, sys_*)
      fd/                   File descriptors (fd table, tty, pipe)
      vfs/                  Virtual filesystem (mount table, path resolution)
      fs/                   Filesystem drivers (romfs, devfs, procfs, vfat, ufs, tmpfs)
      blkdev/               Block device layer (registry, RAM, SD, loopback)
      exec/                 ELF loader + execve
      signal/               Signal infrastructure
    drivers/                Hardware drivers (UART, SPI, LCD, I2C, etc.)
  src/user/                 User-space programs + per-arch build rules
    arch/arm_m/             ARM: crt0.S, syscall.S, user.ld
    arch/m68k/              m68k: crt0.S, syscall.S, user.ld
  tests/
    kernel/                 On-target kernel integration tests (ktest.c)
    host/                   Host-native unit tests (test_kmem, test_fd, test_elf)
    user/                   User-space test programs (test_exec, test_pipe, etc.)
  tools/
    mkromfs/                Host tool: generate romfs.bin image
    mkufs/                  Host tool: generate UFS filesystem image
    mkfatimg/               Host tool: generate FAT32 test image
    uf2sanitize.py          Post-process UF2 for PicoCalc bootloader
  third_party/
    pico-sdk/               git submodule — Raspberry Pi Pico SDK (ARM targets)
    musl/                   git submodule — musl libc v1.2.5
    busybox/                git submodule — busybox 1_36_1
    rogue/                  git submodule — Rogue 5.4.4 (Davidslv/rogue)
    patches/
      musl/                 musl overlay + PIE linker scripts (libc_arm_m.ld, libc_m68k.ld)
      busybox/              busybox config fragments
      rogue/                rogue curses shim + header overrides
    build_musl.sh           Build script: musl libc.a (ARM and m68k)
    build_busybox.sh        Build script: static busybox binary
    build_rogue.sh          Build script: Rogue with minimal curses shim
  src/etc/                  Root filesystem config templates (fstab, passwd, …)
  scripts/
    setup-toolchain.sh      One-shot toolchain install
    build.sh                Build any target (pico1, pico1calc, qemu_arm, qemu_m68k)
    run.sh                  Flash or run any target
    test.sh                 Run tests (--all for full suite)
  docs/
    spec-v07.md             Full design specification
    kernel.md               Kernel internals (boot, memory, scheduler, signals)
    filesystems.md          VFS layer and filesystem drivers
    syscall.md              System call reference (shared across architectures)
    procfs.md               /proc filesystem specification
    userland-dev-guide.md   User-space development guide
    porting.md              Third-party application porting guide
    target-68000.md         m68k target-specific notes
    target-pizero.md        Pi Zero target notes
    reference/              Hardware reference documents (PicoCalc, LCD)
    history/                Development phase plans and porting notes
```

## Quick Start

### 1. Install the toolchain

```sh
./scripts/setup-toolchain.sh
```

Installs apt packages (`arm-none-eabi-gcc`, `cmake`, `ninja`, `openocd`, `gdb-multiarch`, `qemu-system-arm`) and initializes git submodules (Pico SDK, musl, busybox, etc.). For m68k targets, see also `scripts/build-m68k-toolchain.sh`.

### 2. Build

```sh
./scripts/build.sh pico1calc           # build a single target
./scripts/build.sh --test qemu_arm     # build with tests enabled
./scripts/build.sh qemu_m68k           # build m68k QEMU target
```

Or invoke CMake directly (each target is a standalone project):

```sh
# ARM QEMU target (needs explicit toolchain file)
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain_arm_m.cmake \
      -S src/target/qemu_arm -B build/qemu_arm
cmake --build build/qemu_arm

# Pico targets (Pico SDK auto-detected)
cmake -S src/target/pico1calc -B build/pico1calc
cmake --build build/pico1calc

# m68k targets
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain_m68k.cmake \
      -S src/target/qemu_m68k -B build/qemu_m68k
cmake --build build/qemu_m68k
```

### 3. Run / Flash

```sh
./scripts/run.sh                        # run qemu_arm (must be pre-built)
./scripts/run.sh --build                # build & run qemu_arm
./scripts/run.sh --build qemu_m68k      # build & run m68k
./scripts/run.sh --test                 # build with tests, run & check results
./scripts/run.sh --test qemu_m68k       # same for m68k
./scripts/run.sh --gdb                  # run existing binary under GDB
./scripts/run.sh pico1calc              # flash pre-built pico1calc via OpenOCD
./scripts/run.sh --build pico1calc      # build & flash pico1calc
./scripts/run.sh --test pico1           # build with tests & flash pico1
```

**UF2 (no debug adapter):** Hold BOOTSEL (Pico) or bootloader key (PicoCalc)
during power-on, then copy the UF2:

```sh
cp build/pico1calc/ppap_pico1calc.uf2 /media/$USER/RPI-RP2/
```

At the shell prompt, try `rogue` to play the classic dungeon crawler.

Press **Ctrl-A X** to quit QEMU.

### 4. Test

```sh
./scripts/test.sh            # host unit tests only
./scripts/test.sh --all      # host + build all targets + QEMU tests (ARM & m68k)
```

### 5. Debug with GDB (ARM hardware)

```sh
# Terminal 1 — start OpenOCD
openocd -f scripts/debug/openocd.cfg

# Terminal 2 — flash and debug PicoCalc
gdb-multiarch -x scripts/debug/pico1calc.gdb build/pico1calc/ppap_pico1calc.elf

# Or attach to already-running firmware
gdb-multiarch -x scripts/debug/pico1calc-attach.gdb build/pico1calc/ppap_pico1calc.elf
```

## Architecture-Specific Notes

### ARM (RP2040) — Flash Memory Layout

**pico1** (Official Raspberry Pi Pico — 2 MB flash):

| Region | Address | Size | Contents |
|---|---|---|---|
| `FLASH_BOOT` | `0x10000000` | 4 KB | SDK boot2 (256 B) + `stage1.S` (VTOR redirect) |
| `FLASH_KERNEL` | `0x10001000` | 80 KB | Vector table + `.text` + `.rodata` |
| `FLASH_ROMFS` | `0x10015000` | ~1.9 MB | romfs image |

**pico1calc** (ClockworkPi PicoCalc — 16 MB flash):

| Region | Address | Size | Contents |
|---|---|---|---|
| `FLASH_BOOT` | `0x10000000` | 16 KB | SDK boot2 (256 B) + `stage1.S` (reserved by UF2 bootloader) |
| `FLASH_KERNEL` | `0x10004000` | 96 KB | Vector table + `.text` + `.rodata` |
| `FLASH_ROMFS` | `0x1001C000` | ~16 MB | romfs image |

The PicoCalc uses a third-party UF2 bootloader ([pelrun/uf2loader](https://github.com/pelrun/uf2loader))
that reserves the first 16 KB of flash. The build system automatically sanitizes the
UF2 output to exclude this region (`tools/uf2sanitize.py`).

### ARM (RP2040) — SRAM Layout

| Region | Address | Size | Purpose |
|---|---|---|---|
| Kernel data | `0x20000000` | 20 KB | BSS, stack, globals, `.ramfunc` copy |
| Page pool | `0x20005000` | 204 KB | User process pages  |
| I/O buffer | `0x20038000` | 24 KB | SD / FS cache  |
| DMA / Reserved | `0x2003E000` | 16 KB | DMA, PIO, Core 1 stack |

### m68k (QEMU virt)

The m68k target runs on QEMU's `virt` machine with a pure 68000 CPU. Available
RAM is auto-detected at boot via a two-phase probe (1 MB coarse + 4 KB fine
steps), supporting up to 16 MB (`PAGE_COUNT_MAX=4096`). The kernel, romfs, and
page pool all reside in RAM.

| Region | Address | Size | Purpose |
|---|---|---|---|
| Vector table | `0x00000000` | 1 KB | 68000 exception vectors |
| Kernel | `0x00000400` | ~variable | `.text` + `.rodata` + `.data` + `.bss` |
| Kernel stack | after BSS | 16 KB | Supervisor stack |
| Page pool | after stack (4 KB aligned) | runtime-detected | User process pages |

See [docs/target-68000.md](docs/target-68000.md) for architecture-specific details.