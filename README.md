# PicoPiAndPortable (PPAP)

A UNIX-like micro OS for the RP2040 — bare-metal, no SDK runtime.

> Full design specification: [docs/PicoPiAndPortable-spec-v02.md](docs/PicoPiAndPortable-spec-v02.md)

---

## Goals

- Root file system (`/bin`, `/sbin`, `/etc`) on external QSPI flash as **romfs**, executed via XIP
- SD card mounted at `/usr` for additional apps and user data
- POSIX-subset system call interface
- Run **busybox** (statically linked) with an interactive `ash` shell
- Clean porting path to RP2350 (Cortex-M33 + enhanced MPU)

## Hardware

| | RP2040 (current) | RP2350 (future) |
|---|---|---|
| CPU | Dual Cortex-M0+ @ 133 MHz | Dual Cortex-M33 @ 150 MHz |
| SRAM | 264 KB | 520 KB |
| Flash | 2–16 MB QSPI (XIP) | Same + optional PSRAM |
| MPU | 4 regions | 8 regions + TrustZone |

## Project Status

**Phase 0 — Environment Setup: complete** ✓

| Step | Description | Status |
|---|---|---|
| 1 | ARM cross-toolchain (`arm-none-eabi-gcc 13`, `cmake`, `openocd`) | ✓ |
| 2 | CMake build producing `ppap.elf` / `ppap.bin` / `ppap.uf2` | ✓ |
| 3 | Custom linker script — final flash layout | ✓ |
| 4 | `startup.S` — vector table, Reset_Handler, `.data`/`.bss`/`.ramfunc` copy | ✓ |
| 5 | `uart.c` — bare-metal UART0 at 115200 bps | ✓ |
| 6 | OpenOCD + GDB debug workflow | ✓ |
| 7 | PLL_SYS → 133 MHz | ✓ |
| 8 | `stage1.S` — VTOR redirect to kernel at `0x10001000` | ✓ |
| 9 | XIP verification — flash/SRAM benchmark (1.003× ratio) | ✓ |
| 10 | `scripts/flash.sh` — one-command OpenOCD flash | ✓ |
| 11 | QEMU smoke test (`mps2-an500`, `ppap_qemu` target) | ✓ |

## Repository Layout

```
PPAP/
  CMakeLists.txt            Build system (ppap + ppap_qemu targets)
  ldscripts/
    ppap.ld                 Flash layout: boot2 4KB @ 0x10000000, kernel 64KB @ 0x10001000
    qemu.ld                 QEMU layout: ROM @ 0x0, RAM @ 0x20000000
  src/
    boot/
      startup.S             Vector table + Reset_Handler
      stage1.S              Stage 1: sets VTOR=0x10001000, jumps to kernel
    kernel/
      main.c                kmain() — RP2040 entry point
      main_qemu.c           kmain() — QEMU entry point (no PLL/clock init)
      xip_test.c/h          XIP verification + SysTick benchmark
    drivers/
      uart.c/h              RP2040 PL011 UART0 driver
      uart_qemu.c           QEMU CMSDK UART driver (0x40004000)
      clock.c/h             PLL_SYS → 133 MHz
  scripts/
    setup-toolchain.sh      One-shot toolchain install (apt + Pico SDK clone)
    flash.sh                Flash via OpenOCD (stops any running debug session)
    qemu.sh                 Run QEMU smoke test (--build, --gdb flags)
  docs/
    PicoPiAndPortable-spec-v02.md   Full design specification
    phase0-plan.md                  Phase 0 detailed notes and results
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
| Kernel data | `0x20000000` | 16 KB | BSS, stack, globals, `.ramfunc` copy |
| Page pool | `0x20004000` | 208 KB | User process pages (Phase 1+) |
| I/O buffer | `0x20038000` | 24 KB | SD / FS cache (Phase 2+) |
| DMA / Reserved | `0x2003E000` | 16 KB | DMA, PIO, Core 1 stack |
