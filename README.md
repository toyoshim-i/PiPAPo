# PicoPiAndPortable (PPAP)

A UNIX-like micro OS for the RP2040 — bare-metal, no SDK runtime.

> Full design specification: [docs/PicoPiAndPortable-spec-v06.md](docs/PicoPiAndPortable-spec-v06.md)

---

## Goals

- Root file system (`/bin`, `/sbin`, `/etc`) on external QSPI flash as **romfs**, executed via XIP
- SD card as **VFAT (FAT32)** for PC/Mac interoperability; UFS image files mounted via **loopback** for full UNIX semantics
- POSIX-subset system call interface
- Run **busybox** (statically linked) with an interactive `ash` shell
- Run **Rogue 5.4.4** (classic dungeon crawler) via a minimal VT100 curses shim
- Three build targets: `qemu` (testing), `pico1` (official Pico, romfs-only), `pico1calc` (PicoCalc with SD)
- Clean porting path to RP2350 (Cortex-M33 + enhanced MPU)

## Hardware

| | RP2040 (current) | RP2350 (future) |
|---|---|---|
| CPU | Dual Cortex-M0+ @ 133 MHz | Dual Cortex-M33 @ 150 MHz |
| SRAM | 264 KB | 520 KB |
| Flash | 2–16 MB QSPI (XIP) | Same + optional PSRAM |
| MPU | 4 regions | 8 regions + TrustZone |

## Project Status

| Phase | Description | Status |
|---|---|---|
| 0 | Environment Setup — toolchain, UART, bootloader, XIP verification | ✓ Complete |
| 1 | Kernel Foundation — memory management, context switch, scheduler, syscalls, MPU, ELF loader | ✓ Complete |
| 2 | romfs + VFS — mkromfs tool, romfs driver, VFS layer, devfs, procfs | ✓ Complete |
| 3 | Process Execution — ELF loader, vfork/exec, pipe, signals, on-target tests | ✓ Complete |
| 4 | SD + VFAT — SPI driver, SD card init, FAT32 read/write | ✓ Complete |
| 5 | UFS + Loopback — UFS driver, loopback block device, fstab mounts | ✓ Complete |
| 6 | musl + busybox — musl porting, busybox build, interactive ash shell | ✓ Complete |
| 7 | Target Support Packages — per-target directories, 3 build targets, test cleanup | ✓ Complete |
| 8 | PIE Binary Optimization — reduce SRAM footprint for user-space binaries | ✓ Complete |
| 9 | Dual-Core Scheduling — hardware spinlocks, per-core state, Core 1 execution | ✓ Complete |
| 10 | Stabilization — process lifecycle, memory safety, FS correctness, signal delivery | ✓ Complete |
| — | **Rogue 5.4.4** — classic dungeon crawler ported with minimal curses shim | ✓ Complete |
| 11 | PicoCalc Devices — LCD, keyboard, audio drivers | Planned |
| 12 | RP2350 Port — MPU 8-region, PSRAM, Thumb-2 optimization | Planned |

## Repository Layout

```
PPAP/
  CMakeLists.txt            Build system (3 targets: ppap_qemu_arm, ppap_pico1, ppap_pico1calc)
  src/
    target/
      target.h              Target abstraction API (5-function interface)
      qemu_arm/             QEMU mps2-an500: CMSDK UART, RAM block device
        qemu.ld             QEMU layout: ROM @ 0x0, RAM @ 0x20000000
      pico1/                Official Raspberry Pi Pico: romfs-only, no SD
        pico1.ld            Official Pico: 2 MB flash, 80 KB kernel @ 0x10001000
      pico1calc/            ClockworkPi PicoCalc: SPI SD card, 16 MB flash
        pico1calc.ld        PicoCalc: 16 MB flash, 96 KB kernel @ 0x10004000
    boot/
      startup.S             Vector table + Reset_Handler
      stage1.S              Stage 1: sets VTOR, jumps to kernel
    kernel/
      main.c                Unified kmain() — uses target hooks for all platforms
      mm/                   Memory management (page allocator, kmem, MPU, XIP)
      proc/                 Process management (PCB, scheduler, context switch)
      syscall/              System call layer (SVC handler, dispatch, sys_*)
      fd/                   File descriptors (fd table, tty, pipe)
      vfs/                  Virtual filesystem (mount table, path resolution)
      fs/                   Filesystem drivers (romfs, devfs, procfs, vfat, ufs, tmpfs)
      blkdev/               Block device layer (registry, RAM, SD, loopback)
      exec/                 ELF loader + execve
      signal/               Signal infrastructure
    drivers/
      uart.c/h              RP2040 PL011 UART0 driver (IRQ mode)
      clock.c/h             PLL_SYS → 133 MHz
      spi.c/h               SPI0 PL022 driver (SD card)
  user/                     User-space programs (ARM Thumb ELFs) + build Makefile
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
    musl/                   git submodule — musl libc v1.2.5
    busybox/                git submodule — busybox 1_36_1
    rogue/                  git submodule — Rogue 5.4.4 (Davidslv/rogue)
    patches/                PPAP-specific patches (musl, busybox, rogue curses shim)
    configs/                Build configs (busybox defconfig, linker script)
    build-musl.sh           Build script: musl libc.a for ARMv6-M
    build-busybox.sh        Build script: static busybox binary
    build-rogue.sh          Build script: Rogue with minimal curses shim
  romfs/                    Root filesystem template (/bin, /etc, /dev)
  scripts/
    setup-toolchain.sh      One-shot toolchain install
    flash.sh                Flash ppap_pico1calc via OpenOCD
    qemu.sh                 Run ppap_qemu_arm on mps2-an500
    test_all_targets.sh     Build all targets + run QEMU automated tests
  docs/
    PicoPiAndPortable-spec-v06.md   Full design specification
    phase00-plan.md .. phase10-plan.md  Phase detailed plans
    port-rogue.md               Rogue porting plan and audit
```

## Quick Start

### 1. Install the toolchain

```sh
./scripts/setup-toolchain.sh
```

Installs: `arm-none-eabi-gcc`, `cmake`, `ninja`, `openocd`, `gdb-multiarch`, `qemu-system-arm`, Pico SDK.

### 2. Configure and build

```sh
cmake -B build
cmake --build build          # builds ppap_qemu_arm, ppap_pico1, ppap_pico1calc
```

### 3. Flash to hardware

```sh
./scripts/flash.sh                     # flash build/ppap_pico1calc.elf via OpenOCD
./scripts/flash.sh --build             # rebuild first, then flash
./scripts/flash.sh build/ppap_pico1.elf  # flash a specific target
```

Or drag the UF2 onto the RP2040 in BOOTSEL mode:
`cp build/src/target/pico1calc/ppap_pico1calc.uf2 /media/$USER/RPI-RP2/`

### 4. QEMU

```sh
./scripts/qemu.sh            # run build/ppap_qemu_arm.elf — boots to BusyBox ash shell
./scripts/qemu.sh --build    # rebuild first, then run
```

At the shell prompt, try `rogue` to play the classic dungeon crawler.

Press **Ctrl-A X** to quit QEMU.

### 5. Run all tests

```sh
./scripts/test_all_targets.sh   # build all targets + QEMU automated tests
```

### 6. Debug with GDB

```sh
# Terminal 1 — start OpenOCD
openocd -f openocd.cfg

# Terminal 2 — flash and debug PicoCalc
gdb-multiarch -x pico1calc.gdb build/ppap_pico1calc.elf

# Or attach to already-running firmware
gdb-multiarch -x pico1calc-attach.gdb build/ppap_pico1calc.elf
```

## Flash Memory Layout

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

## SRAM Layout

| Region | Address | Size | Purpose |
|---|---|---|---|
| Kernel data | `0x20000000` | 20 KB | BSS, stack, globals, `.ramfunc` copy |
| Page pool | `0x20005000` | 204 KB | User process pages (Phase 1+) |
| I/O buffer | `0x20038000` | 24 KB | SD / FS cache (Phase 2+) |
| DMA / Reserved | `0x2003E000` | 16 KB | DMA, PIO, Core 1 stack |
