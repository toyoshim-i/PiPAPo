# PiPAPo (PPAP)

A portable UNIX-like micro OS for bare-metal microcontrollers and retro CPUs.

> Documentation index: [docs/README.md](/docs/README.md)  
> Full design specification: [docs/spec_v07.md](/docs/spec_v07.md)

---

## Goals

- POSIX-subset system call interface — same syscall numbers across all architectures
- Native **push** shell and POSIX-flavoured core utilities (~50 applets, all written from scratch — no busybox)
- Run **Rogue 5.4.4** (classic dungeon crawler) via a minimal VT100 curses shim
- Root file system on flash/ROM as **romfs**; SD card as **VFAT (FAT32)** with **UFS** loopback images
- Multiple target architectures and boards from a shared kernel codebase
- **PicoCalc standalone**: embedded LCD console (40×20 / 80×40 / 40×40), I2C keyboard — no host PC required

## Supported Targets

| Target | Board / Emulator | Architecture | CPU | RAM | Status |
|---|---|---|---|---|---|
| `qemu_arm` | QEMU mps2-an500 | ARM Cortex-M3 | ARMv7-M | 4 MB | 24/24 tests |
| `pico1` | Raspberry Pi Pico | ARM Cortex-M0+ | Dual-core @ 133 MHz | 264 KB | Stable |
| `pico1calc` | ClockworkPi PicoCalc | ARM Cortex-M0+ | Dual-core @ 133 MHz | 264 KB | Stable |
| `pico2` | Raspberry Pi Pico 2 | ARM Cortex-M33 | Dual-core @ 150 MHz | 520 KB | Stable |
| `pico2rv` | Raspberry Pi Pico 2 | RISC-V Hazard3 | RV32IMAC @ 150 MHz | 520 KB | Stable |
| `qemu_rv32` | QEMU virt rv32 | RISC-V | RV32IMAC | 1 MB | Kernel 69/87, user partial |
| `qemu_m68k` | QEMU virt m68k | Motorola 68000 | m68000 | Up to 16 MB | 19/19 tests |
| `x68k` | XEiJ emulator | Motorola 68000 | m68000 @ 10 MHz | 2+ MB | Boots to scheduler |
| `xtensa_cc` | M5Stack CardComputer | Xtensa LX7 | ESP32-S3 @ 240 MHz | 512 KB | Boots, user-space WIP |
| `pcxt` | QEMU / DOSBox-X | Intel 8086 | i8086 real mode | 640 KB | Almost stable |

All targets share the same kernel source, syscall interface, VFS, and process model. Only drivers, boot sequences, linker scripts, and architecture-specific code (context switch, syscall trap) differ per target.

On i16 (PC/XT), the kernel is split into separate code-segment modules (core + VFS) with far-call stubs at the boundaries. See [docs/kernel/modules.md](/docs/kernel/modules.md) and [docs/targets/ia16.md](/docs/targets/ia16.md).

## Features

- **Kernel** — preemptive scheduler, vfork/exec, signals, pipes, memory protection
- **File systems** — romfs, VFAT (SD card), UFS (loopback images), devfs, procfs, tmpfs
- **User space** — push shell, native utilities (ps, ls, cat, df, grep, sed, sort, getty, pdb, …), in-tree libc, Rogue 5.4.4
- **5 architectures** — ARM Cortex-M, RISC-V (RV32IMAC), Motorola 68000, Xtensa LX7, Intel 8086
- **eCPU emulators** — Z80 and m68k software interpreters enable cross-architecture binary execution (CP/M .COM, Human68k .X/.R, S-OS .OBJ)
- **Subsystems** — Human68k, CP/M, S-OS SWORD — run retro OS binaries via syscall bridge
- **PicoCalc display** — SPI LCD framebuffer console (40×20 / 80×40 / 40×40), VT100/ANSI color emulator
- **PicoCalc keyboard** — I2C STM32 co-processor, full keymap with function keys
- **Multi-TTY** — serial console + LCD console with getty login on each
- **PIE/PIC binaries** — position-independent ELFs; XIP text from flash/romfs on ARM and RISC-V (ePIC)

## Known Issues

- **SD card disabled** — SD/VFAT support is tentatively disabled in the current build
- **RISC-V trace tool** — vfork in the trace tool crashes (context switch issue); see [docs/targets/rv32.md](/docs/targets/rv32.md)
- **Xtensa port WIP** — CardComputer (ESP32-S3) boots, romfs mounts, PID 1 loads; user-space write() output not yet visible; see [docs/targets/xtensa.md](/docs/targets/xtensa.md)
- **X68000 port WIP** — boots to scheduler under XEiJ; user-space and Human68k subsystem still in progress

## Future Work

- **Pi Zero Port** — ARM1176JZF-S with full MMU, SD card boot; see [docs/proposals/pizero_port.md](/docs/proposals/pizero_port.md)
- **MS-DOS Subsystem** — INT 21h translation layer for running DOS .COM/.EXE binaries; see [docs/proposals/msdos_subsystem.md](/docs/proposals/msdos_subsystem.md)
- **i8086 eCPU** — software 8086 emulator for cross-architecture DOS binary execution; see [docs/proposals/i8086_ecpu.md](/docs/proposals/i8086_ecpu.md)
- **GDB RSP Stub** — in-kernel GDB stub for source-level debugging over UART without a debug probe; see [docs/proposals/gdb_rsp_stub.md](/docs/proposals/gdb_rsp_stub.md)
- **M33 MPU Full Protection** — per-process data protection using remaining MPU regions; see [docs/proposals/m33_mpu_full_protection.md](/docs/proposals/m33_mpu_full_protection.md)
- **RISC-V PMP** — configure PMP entries for user/kernel memory protection
- **PicoCalc 2** — RP2350 + PicoCalc hardware, dual architecture (ARM/RISC-V)
- Audio driver support

## Repository Layout

```
PPAP/
  cmake/          Shared cmake modules (arm_m, m68k, kernel, user)
  src/            Kernel, arch overlays, target overlays, user programs
  tests/          Kernel integration tests, host unit tests, user-space tests
  tools/          Host tools (mkromfs, mkufs, mkfatimg, font converters)
  third_party/    git submodules (pico-sdk, rogue, esp-idf)
  scripts/        setup.sh, build.sh, run.sh, test.sh, pre-build checks
  docs/           Design spec, kernel internals, target notes, proposals
```

For the full `src/` tree layout (kernel modules, arch overlays, include conventions),
see [docs/getting_started/source_tree.md](/docs/getting_started/source_tree.md).

## Quick Start

**Host prerequisites:** Docker, bash, python3, clang-format (all installed by `./scripts/setup.sh`).

### 1. Install the toolchain

```sh
./scripts/setup.sh arm        # build only the ARM image
./scripts/setup.sh all        # build all target images
```

Builds Docker images containing the cross-compiler, emulator, and build tools for each target family. Requires Docker. See `docker/*/Dockerfile` for image contents.

### 2. Build

```sh
./scripts/build.sh all                 # build all targets sequentially
./scripts/build.sh pico1calc           # build a single target
./scripts/build.sh --test qemu_arm     # build with tests enabled
./scripts/build.sh qemu_m68k           # build m68k QEMU target
./scripts/build.sh pico2rv             # build RISC-V Pico 2 target
./scripts/build.sh qemu_rv32           # build RISC-V QEMU target
./scripts/build.sh xtensa_cc           # build Xtensa CardComputer target
./scripts/build.sh pcxt               # build PC/XT i8086 target
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

# ARM semihosting (serial I/O via debugger instead of UART)
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain_arm_m.cmake \
      -DPPAP_SEMIHOST=ON \
      -S src/target/qemu_arm -B build/qemu_arm
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

See [docs/getting_started/testing.md](/docs/getting_started/testing.md) for the full test guide.

```sh
./scripts/test.sh            # host unit tests only
./scripts/test.sh --all      # host + build all targets + QEMU tests (ARM, m68k, rv32)
```

### 5. Debug

See [docs/getting_started/debugging.md](/docs/getting_started/debugging.md) for GDB setup, OpenOCD, and target-specific instructions.

## Architecture-Specific Notes

- **ARM Cortex-M** — [docs/targets/arm_m.md](/docs/targets/arm_m.md) — flash/SRAM layouts, RP2040/RP2350 details, PicoCalc UF2 bootloader
- **Motorola 68000** — [docs/targets/m68k.md](/docs/targets/m68k.md) — QEMU virt memory map, X68000 floppy boot
- **RISC-V** — [docs/targets/rv32.md](/docs/targets/rv32.md) — RV32IMAC specifics, XIP limitations, known test failures
- **Xtensa** — [docs/targets/xtensa.md](/docs/targets/xtensa.md) — ESP-IDF integration, IRAM/DRAM split, call0 ABI
- **Intel 8086** — [docs/targets/ia16.md](/docs/targets/ia16.md) — real-mode segment split, UFS floppy boot, kernel module system
