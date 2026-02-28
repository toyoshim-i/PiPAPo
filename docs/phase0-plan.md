# Phase 0: Environment Setup — Detailed Plan

**PicoPiAndPortable Development Roadmap**
Estimated Duration: 2 weeks

---

## Goals

Establish a complete, working development environment and verify the foundational hardware behaviors that all subsequent phases depend on.

**Exit Criteria (all must pass before moving to Phase 1):**
- UART console output works at 115200 bps from C code
- Stage 1 bootloader configures QSPI and enables XIP
- A function placed in flash executes correctly via XIP
- System clock runs at 133 MHz
- OpenOCD + GDB can attach, halt, step, and inspect memory
- CMake build produces a flashable UF2 image

---

## Week 1: Toolchain, Build System, and UART

### ✓ Step 1 — Install the Toolchain

Install the ARM cross-compiler and supporting tools:

```
arm-none-eabi-gcc  13.2.1
arm-none-eabi-gdb  (via gdb-multiarch 15.0)
cmake              3.28.3
ninja-build
openocd            0.12.0
minicom
```

Pico SDK cloned to `~/pico-sdk`; `PICO_SDK_PATH` set in `~/.bashrc` and `~/.zshrc`.
Reproducible setup script: `scripts/setup-toolchain.sh`.

### ✓ Step 2 — Project Skeleton and CMake Build

Project structure:

```
PPAP/
  CMakeLists.txt
  pico_sdk_import.cmake
  src/
    boot/startup.S          # Vector table + Reset_Handler
    kernel/main.c           # kmain() entry point
    drivers/uart.c / uart.h # Bare-metal UART0 driver
  ldscripts/ppap.ld         # Custom linker script
  scripts/setup-toolchain.sh
```

Build produces `ppap.elf`, `ppap.bin`, `ppap.uf2`.

### ✓ Step 3 — Linker Script (Flash Memory Layout)

> **Note — Interim layout (Steps 4–7):**
> The RP2040 boot ROM hardcodes reading the application vector table from
> `XIP_BASE + 0x100 = 0x10000100` after boot2 returns.  While we use the
> Pico SDK's 256-byte boot2, the kernel must start at `0x10000100`.
> Step 8 replaces boot2 with our own 4 KB Stage 1 and restores the
> final layout from the design spec (kernel at `0x10001000`).

Current (interim) flash layout:

| Region | Flash Address | Size | Purpose |
|---|---|---|---|
| boot2 (Pico SDK) | `0x10000000` | 256 B | QSPI init; loaded by boot ROM |
| Kernel `.text` / `.rodata` | `0x10000100` | 64 KB | XIP-executed kernel code + vector table |
| romfs image | `0x10010100` | ~16 MB remainder | Read-only file system (Phase 2+) |

Final layout (after Step 8):

| Region | Flash Address | Size | Purpose |
|---|---|---|---|
| Stage 1 bootloader | `0x10000000` | 4 KB | QSPI init + sets VTOR, jumps to kernel |
| Kernel `.text` / `.rodata` | `0x10001000` | 64 KB | XIP-executed kernel code |
| romfs image | `0x10011000` | ~16 MB remainder | Read-only file system (Phase 2+) |

SRAM regions:

| Region | Address | Size | Purpose |
|---|---|---|---|
| Kernel data | `0x20000000` | 16 KB | BSS, stack, globals |
| Page pool | `0x20004000` | 208 KB | User process pages |
| I/O buffer | `0x20038000` | 24 KB | SD / FS cache |
| DMA / Reserved | `0x2003E000` | 16 KB | DMA, PIO, Core 1 stack |

**boot2 double-link pitfall:** `target_link_libraries(ppap bs2_default_library)` causes
CMake to emit the `.o` file twice (512 B in `.boot2`, overflowing 256-byte `FLASH_BOOT`).
Fix: `target_sources(ppap PRIVATE $<TARGET_OBJECTS:bs2_default_library>)`.

### ✓ Step 4 — Startup Code (`startup.S`)

Minimal ARM Thumb startup in `src/boot/startup.S`:

1. `.vectors` section placed at `0x10000100` (interim; `0x10001000` in final layout).
   First two words: `__stack_top` (initial SP) and `Reset_Handler` address (Thumb bit set).
   Cortex-M0+ table: 16 system exception slots + 26 IRQ handlers → `Default_Handler`.
2. `Reset_Handler`:
   - Copy `.data` from flash LMA to SRAM VMA
   - Zero `.bss`
   - Call `kmain()`
3. `Default_Handler`: infinite loop (catches unexpected exceptions).

No C runtime library — pure assembly.

### ✓ Step 5 — UART Driver (`uart.c`)

Bare-metal UART0 in `src/drivers/uart.c` (no Pico SDK runtime):

1. `clock_switch_to_xosc()`: ROSC → 12 MHz XOSC, then enable `clk_peri`
2. Release `IO_BANK0`, `PADS_BANK0`, `UART0` from reset via atomic CLR alias
3. Baud rate: IBRD=6, FBRD=33 → 115200 bps @ 12 MHz (0.08% error)
4. 8N1 with TX FIFO enabled; GPIO 0 = TX, GPIO 1 = RX (FUNCSEL=2)
5. `uart_putc()`, `uart_puts()` with LF→CRLF expansion

**Verified on hardware** — UART output observed at 115200 bps:
```
PicoPiAndPortable booting...
UART: 115200 bps @ 12 MHz XOSC
```

### Step 6 — OpenOCD + GDB Setup

Create `openocd.cfg` targeting the RP2040 via a Picoprobe or CMSIS-DAP adapter:

```
source [find interface/cmsis-dap.cfg]
source [find target/rp2040.cfg]
adapter speed 5000
```

Verify:
- `openocd` connects and detects the RP2040 (two Cortex-M0+ cores)
- `arm-none-eabi-gdb ppap.elf` can attach, set a breakpoint in `kmain()`, and step through code
- Memory reads at `0x20000000` (SRAM) and `0x10000000` (flash XIP base) work correctly

---

## Week 2: Clock Setup, Stage 1 Bootloader, and XIP Verification

### Step 7 — System Clock at 133 MHz

Configure the RP2040's PLL to run at 133 MHz before UART init (Section 7, Stage 2):

1. Use the crystal oscillator (12 MHz XOSC) as the reference
2. Configure PLL_SYS: VCO = 1596 MHz (12 × 133), post-dividers = 1 × 1 → 133 MHz
3. Switch the system clock source to PLL_SYS
4. Update the UART baud rate divisor after the clock switch (IBRD=72, FBRD=11)

Confirm via UART output: `System clock: 133 MHz`.

Consider using the Pico SDK's `clocks_init()` as a reference, but implement it directly without SDK abstractions to understand the hardware.

### Step 8 — Stage 1 Bootloader (`stage1.S`)

The RP2040 boot ROM loads the first 256 bytes of flash into SRAM and executes it. This Stage 1 code must:

1. Configure the QSPI SSI controller for Quad-SPI (QSPI) mode:
   - Set the clock divider (target: ~31.25 MHz QSPI clock at 133 MHz sys clock, or tune for stability)
   - Configure `SPI_CTRLR0` for Quad Read command (`0xEB`, 6 dummy cycles)
   - Enable XIP mode: set `XIP_CTRL` to enable the XIP cache
2. Verify the configuration by reading a known flash address via XIP and comparing to the expected value
3. Set VTOR to `0x10001000` and branch to the kernel entry point there

**Note:** The Pico SDK ships several `boot2_*.S` files for common flash chips (W25Q080, AT25SF128A, etc.). These are good references, but you must select the correct one for your board's flash chip.

After Stage 1 completes, the entire flash is accessible at `0x10000000`–`0x10FFFFFF` via XIP
and the linker script reverts to the final layout (kernel at `0x10001000`).

### Step 9 — XIP Verification

Place a test function explicitly in flash (via linker section attribute) and call it from `kmain()`:

```c
__attribute__((section(".text.xip_test")))
int xip_add(int a, int b) { return a + b; }
```

Verify via GDB:
- `info address xip_add` shows the address is in the flash XIP range (`0x10001xxx`)
- Disassembly shows Thumb instructions (not a copy in SRAM)
- The function returns the correct result

Measure XIP cache performance qualitatively:
- Run a tight loop from flash and time it with SysTick
- Compare to the same loop copied to SRAM
- Confirm the flash version is within 2–3× of SRAM speed (indicating cache hits)

### Step 10 — Flash Image Build and Flashing Script

Finalize the build pipeline:

1. `CMake` produces `ppap.elf`, `ppap.bin`, and `ppap.uf2`
2. Write `flash.sh` that uses OpenOCD to flash `ppap.bin`:
   ```sh
   openocd -f openocd.cfg \
     -c "program ppap.bin verify reset exit 0x10000000"
   ```
3. Alternatively, `ppap.uf2` can be dragged to the RP2040's USB mass storage (BOOTSEL mode)
4. Document which method to use for daily development vs. release

### Step 11 — QEMU Smoke Test (Optional but Recommended)

QEMU's `raspi` target does not support the RP2040, but the `mps2-an385` (Cortex-M3) or `mps2-an500` (Cortex-M0) machines can run armv6m Thumb code without XIP. This is useful for unit-testing pure-C kernel logic (scheduler, memory manager) without hardware.

Set up a QEMU target:
```sh
qemu-system-arm -M mps2-an385 -cpu cortex-m3 \
  -kernel ppap.elf -nographic -serial stdio
```
Adapt the linker script for the QEMU memory map (SRAM at `0x20000000`, 256 KB). This will be the basis for automated CI tests in later phases.

---

## Deliverables

| Deliverable | Description |
|---|---|
| `src/boot/stage1.S` | Stage 1 bootloader: QSPI init + XIP enable |
| `src/boot/startup.S` | Reset handler, BSS zero, data copy, branch to `kmain` |
| `src/kernel/main.c` | Early kernel entry: clock init, UART init, XIP test |
| `src/drivers/uart.c` | UART driver at 115200 bps |
| `ldscripts/ppap.ld` | Linker script defining flash + SRAM layout |
| `CMakeLists.txt` | Build system producing ELF, BIN, UF2 |
| `openocd.cfg` | OpenOCD configuration for SWD debugging |
| `flash.sh` | One-command flash script |
| `docs/phase0-notes.md` | Notes on flash chip, timing measurements, gotchas |

---

## Known Risks and Mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| Wrong Stage 1 for the board's flash chip | High | Identify the flash chip on the board (read markings or use ROM `flash_devinfo`); use the matching `boot2_*.S` from Pico SDK as a reference |
| XIP instability at 133 MHz | Medium | Start with a lower QSPI clock (e.g., SYS/8 = ~16 MHz), confirm stability, then increase |
| UART GPIO pin conflict (board-specific) | Low | Check the board schematic; default RP2040 UART0 TX/RX is GPIO 0/1, but some boards route differently |
| QEMU not supporting RP2040 peripherals | Certain | Use QEMU only for pure C logic tests; accept that hardware-dependent code requires real hardware |
| OpenOCD connection issues | Low | Use a known-good Picoprobe or Raspberry Pi as SWD adapter; check wiring (SWDIO, SWDCLK, GND) |

---

## References

- [RP2040 Datasheet](https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf) — Chapters 2 (Bus Fabric), 4 (SSI/XIP), 12 (GPIO)
- [Pico SDK boot2 sources](https://github.com/raspberrypi/pico-sdk/tree/master/src/rp2040/boot_stage2) — Reference Stage 1 implementations
- [PicoPiAndPortable Design Spec v0.2](PicoPiAndPortable-spec-v02.md) — Parent design document
