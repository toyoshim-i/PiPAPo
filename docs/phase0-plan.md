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

### ✓ Step 6 — OpenOCD + GDB Setup

`openocd.cfg` with Picoprobe/CMSIS-DAP (default), J-Link, ST-Link, RPi GPIO options.
`ppap.gdb` — flash + debug workflow (uses `load` for reliable reset-halt).
`ppap-attach.gdb` — attach to already-running firmware without reflashing.

**Gotchas resolved:**
- XIP flash is read-only → `gdb_breakpoint_override hard` in openocd.cfg
- Dual-core GDB hang: `set USE_CORE 0` before `source rp2040.cfg`
- `continue` never stopping: `load` required before `monitor reset halt`
- GDB binary is `gdb-multiarch` on Ubuntu (no `gdb-arm-none-eabi` package)

---

## Week 2: Clock Setup, Stage 1 Bootloader, and XIP Verification

### ✓ Step 7 — System Clock at 133 MHz

`src/drivers/clock.c` — `clock_init_pll()`:

1. Switch clk_sys to clk_ref (safe glitchless transition)
2. Reset PLL_SYS via RESETS block
3. REFDIV=1, FBDIV=133 → VCO = 12 × 133 = 1596 MHz
4. Power up VCO; wait for LOCK
5. POSTDIV1=6, POSTDIV2=2 → output = 1596 / 12 = **133 MHz**
6. Power up post-dividers
7. Switch clk_sys to PLL_SYS (via AUX mux)

`uart_flush()` called before PLL switch to drain TX shift register.
`uart_reinit_133mhz()` reconfigures PL011 baud divisors (IBRD=72, FBRD=11).

**Verified on hardware:**
```
PicoPiAndPortable booting...
UART: 115200 bps @ 12 MHz XOSC
System clock: 133 MHz
```

### ✓ Step 8 — Stage 1 Bootloader (`stage1.S`)

**Approach:** hybrid — SDK boot2 handles QSPI init (first 256 bytes with CRC); `stage1.S`
is a 32-byte extension at `0x10000100` that redirects to the kernel at `0x10001000`.

`src/boot/stage1.S` places the `.stage1` section at `0x10000100`:
- Word 0: `__stack_top` — SDK boot2 exit reads this to set MSP
- Word 1: `stage1_entry` (Thumb bit set) — SDK boot2 exit branches here
- `stage1_entry` (8 instructions):
  1. Set VTOR = `0x10001000`
  2. Load `Reset_Handler` from `[0x10001004]`
  3. Load `__stack_top` from `[0x10001000]`; set MSP; branch to `Reset_Handler`

`ldscripts/ppap.ld` updated to the **final layout**:

| Region | Flash Address | Size | Purpose |
|---|---|---|---|
| FLASH_BOOT | `0x10000000` | 4 KB | SDK boot2 (256 B) + stage1 extension (32 B) |
| FLASH_KERNEL | `0x10001000` | 64 KB | Kernel vector table + `.text` / `.rodata` |
| FLASH_ROMFS | `0x10011000` | ~16 MB | romfs (Phase 2+) |

Verified section addresses:
```
.boot2   256 B @ 0x10000000  (SDK boot2 — QSPI init + CRC)
.stage1   32 B @ 0x10000100  (stage1_entry — VTOR redirect)
.vectors 168 B @ 0x10001000  (kernel vector table — final layout ✓)
.text        … @ 0x100010a8  (kernel code)
```

The UART output is unchanged (QSPI and XIP were already working via SDK boot2).

### ✓ Step 9 — XIP Verification

`src/kernel/xip_test.c` — three functions, output from `kmain()`:

1. **`xip_add`** in `.text.xip_test` (XIP flash):  address printed at boot proves `0x10001xxx`.
2. **`xip_bench(10000)`** in `.text.xip_test`: SysTick-timed tight loop from flash.
3. **`sram_bench(10000)`** in `.ramfunc.sram_bench`: identical loop copied to SRAM by
   `Reset_Handler`; compares against flash timing to confirm cache effectiveness.

New linker section `.ramfunc` in `ppap.ld`: VMA in `RAM_KERNEL`, LMA in `FLASH_KERNEL`,
copied to SRAM by `startup.S` alongside `.data`.

`uart_print_hex32()` added to `uart.c`/`uart.h` for printing cycle counts.

**Verified on hardware:**
```
XIP: xip_add @ 0x100011cd
XIP: xip_add(3,4) = 7
XIP: flash bench(10000) = 0x00016098 cycles   (90,264 cycles)
XIP: sram  bench(10000) = 0x00015f9a cycles   (90,010 cycles)
```

Flash/SRAM ratio: **1.003×** — essentially identical.  The XIP cache warms after the
first loop iteration; all subsequent instruction fetches hit the cache, so flash and
SRAM performance are indistinguishable for a hot tight loop.

Key addresses confirmed by `arm-none-eabi-nm`:
```
xip_add   @ 0x100011cc  T  (XIP flash — .text.xip_test) ✓
xip_bench @ 0x100011d0  T  (XIP flash — .text.xip_test) ✓
sram_bench@ 0x20000000  T  (SRAM VMA  — .ramfunc copy)  ✓
__ramfunc_load 0x10001510   (flash LMA, copied to SRAM by startup.S) ✓
```

### ✓ Step 10 — Flash Image Build and Flashing Script

CMake already produces `ppap.elf`, `ppap.bin`, and `ppap.uf2` via
`pico_add_extra_outputs(ppap)`.

`scripts/flash.sh` — one-command flash via OpenOCD:
```sh
./scripts/flash.sh           # flash build/ppap.elf
./scripts/flash.sh --build   # cmake --build first, then flash
```

Internally uses the **ELF** (not `.bin`) so addresses are embedded:
```sh
openocd -f openocd.cfg -c "program build/ppap.elf verify reset exit"
```

| Method | When to use |
|---|---|
| `./scripts/flash.sh` (OpenOCD) | Daily dev — no unplug required; verifies write |
| `ppap.uf2` drag-and-drop | No debug adapter — hold BOOTSEL, plug USB, copy |

### ✓ Step 11 — QEMU Smoke Test

QEMU's `raspi` target does not support the RP2040, but the `mps2-an500` (Cortex-M0+) machine runs ARMv6-M Thumb code without XIP. This provides a fast, hardware-free test loop for pure-C kernel logic (scheduler, memory manager).

New files:

| File | Purpose |
|---|---|
| `ldscripts/qemu.ld` | ROM at `0x00000000` (8 MB), RAM at `0x20000000` (512 KB), stack 16 KB |
| `src/drivers/uart_qemu.c` | CMSDK UART at `0x40004000`; BAUDDIV=217 for 115200 bps @ 25 MHz |
| `src/kernel/main_qemu.c` | No RP2040 clock/PLL; calls `xip_add(3,4)`, prints "QEMU smoke test PASSED" |
| `scripts/qemu.sh` | `--build` (rebuild first), `--gdb` (pause at reset, GDB on :1234) |

CMake target `ppap_qemu` builds successfully using the same arm-none-eabi-gcc toolchain:

```
arm-none-eabi-nm --numeric-sort build/ppap_qemu.elf | grep -E 'vectors|kmain|xip_add|__stack_top'
00000000 T _vectors
0000010c T kmain
00000184 T xip_add
20004000 A __stack_top
```

Run with:
```sh
sudo apt install qemu-system-arm   # one-time install
./scripts/qemu.sh                  # or --build to rebuild first
```

Expected output:
```
PicoPiAndPortable booting (QEMU mps2-an500)...
UART: CMSDK UART0 @ 0x40004000
Clock: emulated (no PLL — skipping clock_init_pll)
XIP: xip_add @ 0x00000185 (ROM, 0x000xxxxx in QEMU)
XIP: xip_add(3,4) = 7
QEMU smoke test PASSED
```

Press **Ctrl-A X** to quit QEMU.

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
