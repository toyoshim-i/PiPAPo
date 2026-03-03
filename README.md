# PicoPiAndPortable (PPAP)

A UNIX-like micro OS for the RP2040 — bare-metal, no SDK runtime.

> Full design specification: [docs/PicoPiAndPortable-spec-v04.md](docs/PicoPiAndPortable-spec-v04.md)

---

## Goals

- Root file system (`/bin`, `/sbin`, `/etc`) on external QSPI flash as **romfs**, executed via XIP
- SD card as **VFAT (FAT32)** for PC/Mac interoperability; UFS image files mounted via **loopback** for full UNIX semantics
- POSIX-subset system call interface
- Run **busybox** (statically linked) with an interactive `ash` shell
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
| 6 | musl + busybox — musl porting, busybox build, interactive ash shell | In progress (Step 6/12) |
| 7 | Board Support Packages — split target code into per-board directories | Planned |
| 8 | Stabilization — error handling, OOM killer, performance tuning | Planned |
| 9 | RP2350 Port — MPU 8-region, PSRAM, Thumb-2 optimization | Planned |

## Repository Layout

```
PPAP/
  CMakeLists.txt            Build system (ppap + ppap_qemu targets)
  ldscripts/
    ppap.ld                 Flash layout: boot2 4KB @ 0x10000000, kernel 64KB @ 0x10001000
    qemu.ld                 QEMU layout: ROM @ 0x0, RAM @ 0x20000000
  src/
    board/
      picocalc.h            PicoCalc GPIO pin definitions (SD card SPI0)
    boot/
      startup.S             Vector table + Reset_Handler
      stage1.S              Stage 1: sets VTOR=0x10001000, jumps to kernel
    kernel/
      main.c                kmain() — RP2040 entry point
      main_qemu.c           kmain() — QEMU entry point
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
      uart.c/h              RP2040 PL011 UART0 driver
      uart_qemu.c           QEMU CMSDK UART driver
      clock.c/h             PLL_SYS → 133 MHz
      spi.c/h               SPI0 PL022 driver (SD card)
  user/                     User-space test programs (ARM Thumb ELFs)
  tests/                    Host-native unit tests
  tools/
    mkromfs/                Host tool: generate romfs.bin image
    mkufs/                  Host tool: generate UFS filesystem image
  third_party/
    musl/                   git submodule — musl libc v1.2.5
    busybox/                git submodule — busybox 1_36_1
    patches/                PPAP-specific patches for musl and busybox
    configs/                Build configs (busybox defconfig)
    build-musl.sh           Build script: musl libc.a for ARMv6-M
    build-busybox.sh        Build script: static busybox binary
  romfs/                    Root filesystem template (/bin, /etc, /dev)
  scripts/
    setup-toolchain.sh      One-shot toolchain install
    flash.sh                Flash via OpenOCD
    qemu.sh                 Run QEMU tests
  docs/
    PicoPiAndPortable-spec-v04.md   Full design specification
    phase0-plan.md .. phase6-plan.md   Phase detailed plans
```

## Quick Start

### 1. Install the toolchain

```sh
./scripts/setup-toolchain.sh
```

Installs: `arm-none-eabi-gcc`, `cmake`, `ninja`, `openocd`, `gdb-multiarch`, `qemu-system-arm`, Pico SDK.

### 2. Configure and build

```sh
mkdir -p build && cd build
cmake ..
cmake --build .          # builds ppap.elf + ppap_qemu.elf
```

### 3. Flash to hardware

```sh
./scripts/flash.sh           # flash build/ppap.elf via OpenOCD
./scripts/flash.sh --build   # rebuild first, then flash
```

Or drag `build/ppap.uf2` onto the RP2040 in BOOTSEL mode.

### 4. QEMU smoke test

```sh
./scripts/qemu.sh            # run build/ppap_qemu.elf on mps2-an500
./scripts/qemu.sh --build    # rebuild first, then run
```

Expected output:
```
PicoPiAndPortable booting (QEMU mps2-an500)...
UART: CMSDK UART0 @ 0x40004000
Clock: emulated (no PLL — skipping clock_init_pll)
XIP: xip_add @ 0x00000185
XIP: xip_add(3,4) = 7
QEMU smoke test PASSED
```

Press **Ctrl-A X** to quit QEMU.

### 5. Debug with GDB

```sh
# Terminal 1 — start OpenOCD
openocd -f openocd.cfg

# Terminal 2 — attach GDB
gdb-multiarch -x ppap.gdb build/ppap.elf
```

## Flash Memory Layout

| Region | Address | Size | Contents |
|---|---|---|---|
| `FLASH_BOOT` | `0x10000000` | 4 KB | SDK boot2 (256 B) + `stage1.S` (VTOR redirect) |
| `FLASH_KERNEL` | `0x10001000` | 64 KB | Vector table + `.text` + `.rodata` |
| `FLASH_ROMFS` | `0x10011000` | ~16 MB | romfs image (Phase 2+) |

## SRAM Layout

| Region | Address | Size | Purpose |
|---|---|---|---|
| Kernel data | `0x20000000` | 20 KB | BSS, stack, globals, `.ramfunc` copy |
| Page pool | `0x20005000` | 204 KB | User process pages (Phase 1+) |
| I/O buffer | `0x20038000` | 24 KB | SD / FS cache (Phase 2+) |
| DMA / Reserved | `0x2003E000` | 16 KB | DMA, PIO, Core 1 stack |
