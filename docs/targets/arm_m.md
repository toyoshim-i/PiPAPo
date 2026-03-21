# ARM Cortex-M Targets

Architecture-specific reference for the PPAP ARM Cortex-M port. Current targets
are QEMU (`mps2-an500`, Cortex-M3), Raspberry Pi Pico (RP2040, Cortex-M0+),
PicoCalc (RP2040 + LCD/keyboard/SD), and Pico 2 (RP2350, Cortex-M33).

---

## 1. Architecture Overview

| Aspect | Cortex-M0+ (RP2040) | Cortex-M3 (QEMU) | Cortex-M33 (RP2350) |
|--------|---------------------|-------------------|---------------------|
| ISA | ARMv6-M (Thumb-1) | ARMv7-M (Thumb-2) | ARMv8-M (Thumb-2) |
| Word size | 32-bit | 32-bit | 32-bit |
| Endianness | Little-endian | Little-endian | Little-endian |
| Registers | r0-r12, SP, LR, PC, xPSR | Same | Same + s0-s31 (FPU) |
| Callee-saved | r4-r11 | r4-r11 | r4-r11, s16-s31 |
| Stack pointer | MSP/PSP (dual) | MSP/PSP | MSP/PSP |
| Privilege | Handler/Thread mode | Same | Same + TrustZone (unused) |
| Syscall | `svc 0` | `svc 0` | `svc 0` |
| Timer | SysTick | SysTick | SysTick |
| Context switch | PendSV (deferred) | PendSV | PendSV |
| MPU | 4-region MPU | Optional | 8-region MPU |
| FPU | None | None | FPv5-SP-D16 (optional) |
| HW breakpoints | FPB v1 (4 code) | FPB v1 | FPB v2 (not yet used) |
| Division | Software (libgcc) | Hardware (`udiv`/`sdiv`) | Hardware |
| Multi-core | Dual-core (SIO FIFO) | Single-core | Dual-core (SIO FIFO) |

---

## 2. Syscall ABI

PPAP uses the ARM EABI Linux syscall convention (compatible with musl libc):

```
svc 0
r7 = syscall number
r0 = arg1 → return value
r1 = arg2
r2 = arg3
r3 = arg4
r4 = arg5
r5 = arg6
```

The hardware exception entry automatically pushes `{r0, r1, r2, r3, r12, lr, pc,
xpsr}` onto PSP. The SVC handler captures `r4`, `r5`, `r7` (callee-saved, still
live) before dispatching to `syscall_dispatch(frame, nr, a4, a5)`.

### Post-Syscall Checks

After dispatch, the SVC handler checks (in order):

1. **exec_pending** — `sys_execve` loaded a new image; do a full PendSV-style
   restore from `current->sp` (r4-r11 including r9/GOT base).
2. **Process blocked** — PendSV already pended by `sched_yield()`; if
   `svc_restart` is set, restore original `frame[0]` and adjust `PC -= 2` so
   the SVC re-executes on reschedule.
3. **Pending signals** — `signal_check()` may modify PSP for signal delivery.

---

## 3. Context Switch

### PCB Register Layout

```
PCB offset  0..31:  r4, r5, r6, r7, r8, r9, r10, r11 (8 regs × 4B = 32B)
PCB offset 32:      sp (saved PSP)
PCB_SP_OFFSET = 32
```

### PendSV Handler

PendSV runs at the lowest exception priority, triggered by SysTick via
`ICSR.PENDSVSET`. It never preempts a real interrupt handler.

**ARMv6-M (Cortex-M0+) path:**

1. `mrs r0, psp` — get outgoing PSP
2. Manual `subs r0, #32` + `stmia` for r4-r7, then move r8-r11 via r4-r7 and
   `stmia` again (ARMv6-M only has STMIA, no STMDB; high registers can't
   appear directly in STMIA)
3. Save PSP into outgoing `pcb_t.sp`
4. Call `sched_next()` → returns next PCB
5. Call `mpu_switch()` + `trace_arm_hwbp_on_switch()`
6. Restore r4-r11 from incoming PCB (reverse dance with push/pop for r8-r11)
7. `msr psp, r0` + `bx lr` (EXC_RETURN)

**ARMv8-M (Cortex-M33) path:**

1. `mrs r0, psp` — get outgoing PSP
2. Check EXC_RETURN bit 4: if FPU was active (`bit4 == 0`), save s16-s31 via
   `vstmdb`
3. `stmdb r0!, {r4-r11, lr}` — save callee-saved + EXC_RETURN (Thumb-2
   supports full register list)
4. Same scheduler/MPU/breakpoint calls
5. `ldmia r0!, {r4-r11, lr}` — restore + EXC_RETURN
6. If EXC_RETURN bit 4 == 0, restore s16-s31 via `vldmia`
7. `msr psp, r0` + `bx lr`

### Stack Layout (Suspended Process)

**ARMv6-M:**

```
[PSP+0 ]  r4          ← pcb_t.sp points here
[PSP+4 ]  r5
  ...
[PSP+28]  r11
[PSP+32]  r0   ─┐
  ...             │  hardware exception frame (8 words)
[PSP+60]  xpsr ─┘
```

**ARMv8-M with FPU:**

```
[PSP+0 ]  r4          ← pcb_t.sp points here
  ...
[PSP+28]  r11
[PSP+32]  EXC_RETURN  (bit 4 encodes FPU frame type)
[PSP+36]  s16         ─┐  (only when EXC_RETURN bit 4 = 0)
  ...                   │  callee-saved FP registers
[PSP+96]  s31         ─┘
[PSP+36 or 100]  r0   ─┐
  ...                    │  hardware exception frame (8 or 26 words)
[PSP+xx]  xpsr         ─┘
```

---

## 4. Boot Sequence

### Pico 1 (RP2040, 2 MB flash)

1. **RP2040 boot ROM** reads boot2 (256 B) from `0x10000000`, loads to SRAM,
   runs it — boot2 initializes QSPI/XIP
2. **boot2 exit stub** reads `{SP, entry}` from `0x10000100` (start of stage1)
3. **stage1.S** disables interrupts, sets `VTOR = 0x10001000`, reads
   `{SP, Reset_Handler}` from kernel vector table, branches to kernel
4. **Reset_Handler** (boot.S) copies `.data` flash→SRAM, zeros `.bss`,
   calls `kmain()`

### PicoCalc (RP2040, 16 MB flash)

Two boot paths share the same stage1.S:

1. **SWD flash path** — identical to Pico 1 (boot2 → stage1 → kernel at
   `0x10004000`)
2. **UF2 bootloader path** — UF2 loader does watchdog reset; on next boot,
   stage3 calls `launch_application_from(XIP_BASE + 0x100)`, reads `{SP, entry}`
   from stage1 header, jumps to `stage1_entry`

The UF2 bootloader reserves the first 16 KB (`0x10000000–0x10003FFF`) and writes
28 bytes of proginfo metadata at `XIP_BASE + 0x110`. Words 4-10 of the stage1
header are reserved for this (filled with `0xFF` = erased state).

### Pico 2 (RP2350, 4 MB flash)

1. **RP2350 boot ROM** scans first 4 KB for a PICOBIN IMAGE_DEF block (no
   boot2 needed — ROM handles QSPI/XIP natively)
2. **picobin_block.S** declares an ARM Secure image with ENTRY_POINT metadata
   pointing to `stage1_entry`
3. **stage1.S** sets `VTOR = 0x10001000`, branches to kernel Reset_Handler
4. **Reset_Handler** copies `.data`, zeros `.bss`, enables FPU (CPACR CP10/CP11
   full access + FPCCR ASPEN/LSPEN for lazy stacking), calls `kmain()`

### QEMU ARM (mps2-an500)

1. QEMU loads ELF directly into memory
2. CPU reads `{SP, Reset_Handler}` from vector table at `0x00000000`
3. No boot ROM, no QSPI, no stage1 — Reset_Handler runs immediately

---

## 5. Pico 1 Target (`pico1`)

### Hardware

| Item | Specification |
|------|---------------|
| CPU | Dual Cortex-M0+ @ 133 MHz |
| Flash | 2 MB QSPI (XIP at 0x10000000) |
| SRAM | 264 KB |
| UART | PL011 @ 0x40034000, GP0 (TX), GP1 (RX), 115200 bps |
| XOSC | 12 MHz |
| PLL | FBDIV=133, PD1=6, PD2=2 → 133 MHz |

### Flash Layout

| Region | Address | Size | Contents |
|--------|---------|------|----------|
| FLASH_BOOT | 0x10000000 | 4 KB | boot2 (256 B) + stage1 |
| FLASH_KERNEL | 0x10001000 | 160 KB | Vector table + kernel .text/.rodata |
| FLASH_ROMFS | 0x10029000 | ~1.9 MB | romfs image |

### SRAM Layout

| Region | Address | Size | Purpose |
|--------|---------|------|---------|
| RAM_KERNEL | 0x20000000 | 24 KB | .data, .bss, stack (4 KB) |
| RAM_PAGES | 0x20006000 | 200 KB | Page pool (50 × 4 KB pages) |
| RAM_IOBUF | 0x20038000 | 24 KB | SD DMA, FS cache |
| RAM_DMA | 0x2003E000 | 16 KB | DMA/PIO/Core 1 stack/IRQ stack |

### Target Files

```
src/target/pico1/
  target_pico1.c    — target hooks (early_init, late_init, etc.)
  pico1.ld          — linker script (2 MB flash)
  CMakeLists.txt    — target build rules
```

---

## 6. PicoCalc Target (`pico1calc`)

### Hardware

| Item | Specification |
|------|---------------|
| CPU | Dual Cortex-M0+ @ 133 MHz (same as Pico 1) |
| Flash | 16 MB QSPI (XIP at 0x10000000) |
| SRAM | 264 KB |
| UART | PL011 @ 0x40034000, GP0 (TX), GP1 (RX), 115200 bps |
| SPI0 (SD) | GP16 (MISO), GP18 (SCK), GP19 (MOSI), GP17 (CS), GP22 (CD) |
| SPI1 (LCD) | GP10 (SCK), GP11 (MOSI), GP13 (CS), GP14 (DC), GP15 (RST) |
| I2C1 (KB) | GP6 (SDA), GP7 (SCL), 10 kHz, STM32 @ addr 0x1F |
| LCD | ST7365P 320×320 RGB565, 33 MHz SPI1 |
| Keyboard | I2C-based via STM32 (backlight, battery monitoring) |

### Flash Layout

| Region | Address | Size | Contents |
|--------|---------|------|----------|
| FLASH_BOOT | 0x10000000 | 16 KB | boot2 + stage1 (reserved by UF2 loader) |
| FLASH_KERNEL | 0x10004000 | 208 KB | Vector table + kernel .text/.rodata |
| FLASH_ROMFS | 0x10038000 | ~16 MB | romfs image |

### SRAM Layout

Same as Pico 1 (264 KB total, 24 KB kernel, 200 KB pages, 24 KB iobuf, 16 KB
DMA).

### Target Files

```
src/target/pico1calc/
  target_pico1calc.c   — target hooks (SPI, SD, LCD, keyboard, I2C init)
  pico1calc.ld         — linker script (16 MB flash, 16 KB boot area)
  CMakeLists.txt       — target build rules
```

### Drivers (shared with pico1/pico2)

```
src/drivers/arch/arm_m/
  uart_rpico.c         — PL011 UART (all RPi targets)
  clock_rpico.c        — PLL_SYS configuration
  spi_rpico.c          — SPI0/SPI1 (PicoCalc SD + LCD)
  spi_lcd_rpico.c      — SPI1 LCD driver (ST7365P)
  i2c_rpico.c          — I2C1 keyboard (STM32 controller)
```

---

## 7. Pico 2 Target (`pico2`)

### Hardware

| Item | Specification |
|------|---------------|
| CPU | Dual Cortex-M33 @ 150 MHz (ARMv8-M) |
| Flash | 4 MB QSPI (XIP at 0x10000000) |
| SRAM | 520 KB |
| UART | PL011 @ 0x40034000, GP0 (TX), GP1 (RX), 115200 bps |
| XOSC | 12 MHz |
| PLL | FBDIV=125, PD1=5, PD2=2 → 150 MHz |
| FPU | FPv5-SP-D16 (single-precision, softfp ABI) |

### Flash Layout

| Region | Address | Size | Contents |
|--------|---------|------|----------|
| FLASH_BOOT | 0x10000000 | 4 KB | stage1 header + PICOBIN IMAGE_DEF |
| FLASH_KERNEL | 0x10001000 | 160 KB | Vector table + kernel .text/.rodata |
| FLASH_ROMFS | 0x10029000 | ~3.8 MB | romfs image |

Note: RP2350 ROM handles QSPI/XIP natively — no boot2 needed. The ROM scans the
first 4 KB for a PICOBIN block.

### SRAM Layout

| Region | Address | Size | Purpose |
|--------|---------|------|---------|
| RAM_KERNEL | 0x20000000 | 24 KB | .data, .bss, stack (4 KB) |
| RAM_PAGES | 0x20006000 | 464 KB | Page pool (116 × 4 KB pages) |
| RAM_IOBUF | 0x2007A000 | 16 KB | FS cache |
| RAM_DMA | 0x2007E000 | 8 KB | DMA buffers |

### FPU Support

- **CPACR**: CP10+CP11 set to full access in Reset_Handler
- **FPCCR**: ASPEN + LSPEN enabled (lazy stacking)
- **ABI**: softfp (float arguments in integer registers, computed in FPU)
- **Context switch**: PendSV checks EXC_RETURN bit 4; saves/restores s16-s31
  only when FPU was active. Hardware lazy-stacks s0-s15 automatically.
- **Signal delivery**: Must handle FPU state correctly for FPU-active processes
  (EXC_RETURN bit 4 encoding)

### SMP (Core 1)

- PSM register at `0x40018004`, bit 24 = proc1 (differs from RP2040's bit 16
  at `0x40010004`)
- Atomic aliases: SET (`+0x2000`), CLR (`+0x3000`)
- Core 1 entry: `core1_sched_entry()` — sets up MPU, SysTick, PSP, idle loop
- Boot handshake via SIO FIFO (same 6-word sequence as RP2040)

### Target Files

```
src/target/pico2/
  target_pico2.c     — target hooks (dual-core M33, no SD)
  picobin_block.S    — RP2350 PICOBIN metadata (ARM Secure image)
  pico2.ld           — linker script (4 MB flash, 520 KB SRAM)
  CMakeLists.txt     — target build rules
```

---

## 8. QEMU Target (`qemu_arm`)

### Machine and CPU

```
qemu-system-arm -M mps2-an500 -serial mon:stdio -nographic -kernel ppap_qemu_arm.elf
```

The `mps2-an500` machine provides a Cortex-M3 with CMSDK peripherals. The
Cortex-M3 (ARMv7-M) ISA is a superset of Cortex-M0+ (ARMv6-M), so the same
kernel Thumb-1 code runs on both.

### Memory Map

| Region | Address | Size | Purpose |
|--------|---------|------|---------|
| ROM | 0x00000000 | 8 MB | Vector table + kernel + romfs + FAT image |
| RAM | 0x20000000 | 512 KB | .data, .bss, stack (16 KB), page pool |
| UART0 | 0x40004000 | 4 KB | CMSDK UART (not PL011) |

### UART

CMSDK UART0 at `0x40004000` (not PL011):

| Register | Offset | Description |
|----------|--------|-------------|
| DATA | +0x00 | TX/RX data |
| STATE | +0x04 | Status (bit 0 = TX full, bit 1 = RX full) |
| CTRL | +0x08 | Control (TX/RX enable, interrupt enable) |
| BAUDDIV | +0x10 | Baud divisor (217 = 25 MHz / 115200) |

IRQ 0 = RX, IRQ 1 = TX.

### Bootloader Detection

The kernel auto-detects the CPU variant via CPUID:

- Cortex-M3 (`CPUID_PARTNO = 0xC23`) → skip `core1_launch()` (no SIO on QEMU)
- Cortex-M0+ → proceed with SIO-based Core 1 launch

No PLL setup, no clock init — QEMU provides a fixed virtual clock (~25 MHz).

### SysTick

SysTick counter is present but TICKINT is unreliable on QEMU mps2-an500.
Cooperative scheduling via `sched_yield()` is used for testing.

### Block Device

QEMU ARM uses a RAM-backed block device from an embedded FAT32 image:

1. `mkfatimg` (host tool) builds a FAT32 image with test files
2. `fatimg_data.S` embeds the image via `.incbin` into the `.fatimg` section
3. Kernel mounts it as the block device at boot

### Target Files

```
src/target/qemu_arm/
  target_qemu_arm.c   — target hooks (RAM block device, FAT32 mount)
  qemu.ld             — linker script (ROM + RAM, no flash)
  CMakeLists.txt       — target build rules
  drivers/
    uart_qemu.c        — CMSDK UART0 driver
```

---

## 9. Shared Architecture Files

```
src/arch/arm_m/
  boot.S             — vector table + Reset_Handler (.data copy, .bss zero, FPU init)
  stage1.S           — stage1 bootloader (VTOR redirect to kernel)
  switch.S           — PendSV_Handler (context switch, FPU lazy stacking)
  trap.S             — SVC_Handler (syscall dispatch, exec restore, restart)
  arm_m_common.c     — ARM M-profile common code
  arch.h             — IRQ save/restore, yield, core_id, WFI
  cpu.h              — exception frame layout, SR bits
```

---

## 10. User-Space Details

### Compiler Flags

```sh
CFLAGS="-mthumb -mcpu=cortex-m0plus -fPIC -msingle-pic-base -mpic-register=r9"
```

The `-msingle-pic-base -mpic-register=r9` flags use r9 as the GOT base register
(analogous to m68k's `-msep-data` with a5).

### ELF Format

| Field | Value |
|-------|-------|
| Class | ELF32 |
| Data | Little-endian (ELFDATA2LSB) |
| Machine | EM_ARM (40) |
| Type | ET_DYN (PIE) |
| Relocation | R_ARM_RELATIVE (23) |

Code executes in place (XIP) from flash on hardware targets. Only writable
`.data`/`.bss` needs per-process SRAM pages.

---

## 11. Key Register Addresses

### RP2040 / RP2350 Peripherals

| Peripheral | Address | Notes |
|------------|---------|-------|
| RESETS | 0x4000C000 | CLR: +0x3000, SET: +0x2000 |
| CLOCKS | 0x40008000 | |
| XOSC | 0x40024000 | 12 MHz reference |
| PLL_SYS | 0x40028000 | |
| IO_BANK0 | 0x40014000 | GPIO function select |
| UART0 | 0x40034000 | PL011 |
| SPI0 | 0x4003C000 | SD card (PicoCalc) |
| SPI1 | 0x40040000 | LCD (PicoCalc) — NOT 0x4003D000! |
| I2C1 | 0x40048000 | Keyboard (PicoCalc) |
| SIO | 0xD0000000 | Core ID, FIFO, spinlocks |
| PSM (RP2040) | 0x40010004 | bit 16 = proc1 |
| PSM (RP2350) | 0x40018004 | bit 24 = proc1 |

**Important**: RP2040/RP2350 peripherals are spaced **16 KB** apart (4 KB × 4
atomic aliases: normal/XOR/SET/CLR). Using base+0x1000 hits the XOR alias of the
previous peripheral (silent corruption).

### QEMU mps2-an500 Peripherals

| Peripheral | Address | Notes |
|------------|---------|-------|
| UART0 | 0x40004000 | CMSDK UART (not PL011) |

---

## 12. Timer and Scheduling

| Target | Clock | SysTick Reload | Time Slice |
|--------|-------|----------------|------------|
| pico1 / pico1calc | 133 MHz | 1,329,999 | 10 ms |
| pico2 | 150 MHz | 1,499,999 | 10 ms |
| qemu_arm | ~25 MHz (virtual) | Unreliable | Cooperative |

Formula: `SYSTICK_RELOAD = (SYS_HZ / 100) - 1`

---

## 13. Build and Run

```sh
# Build
./scripts/run.sh --build pico1        # build Pico 1
./scripts/run.sh --build pico1calc    # build PicoCalc
./scripts/run.sh --build pico2        # build Pico 2
./scripts/run.sh --build qemu_arm     # build QEMU ARM

# Run
./scripts/run.sh pico1                # flash via OpenOCD
./scripts/run.sh pico2                # flash via OpenOCD (RP fork)
./scripts/run.sh qemu_arm             # run in QEMU

# Test
./scripts/run.sh --test qemu_arm      # build + run ARM tests

# Debug
./scripts/run.sh --gdb qemu_arm       # run under GDB on :1234
```

### Toolchain

```
arm-none-eabi-gcc     — bare-metal cross compiler (apt: gcc-arm-none-eabi)
arm-none-eabi-as / arm-none-eabi-ld
gdb-multiarch         — GDB (Ubuntu dropped gdb-arm-none-eabi; symlink via setup-toolchain.sh)
qemu-system-arm       — apt install qemu-system-arm
openocd               — flash/debug for Pico 1 / PicoCalc
openocd (RP fork)     — flash/debug for Pico 2 (RP2350 support)
```

---

## 14. Key Differences from m68k

| Aspect | ARM Cortex-M | m68k (QEMU / X68000) |
|--------|-------------|----------------------|
| Code execution | XIP from flash (hw) / ROM (QEMU) | RAM only |
| Multi-core | Dual-core, HW spinlocks | Single-core |
| Memory protection | MPU (4-8 regions) | None |
| Timer | SysTick (built-in) | External (Goldfish PIT / 8253 / MFP) |
| Context switch | PendSV (deferred lowest priority) | Direct in timer ISR |
| User/kernel | Thread mode + PSP / Handler mode + MSP | User mode (USP) / Supervisor mode (SSP) |
| Division | Software on M0+, hardware on M3/M33 | Hardware (`divs`/`divu`) |
| PIC register | r9 = GOT base | a5 = data base |
| Endianness | Little-endian | Big-endian |

---

## Related Documentation

- [68000.md](/docs/targets/68000.md) — m68k target reference
- [kernel overview](/docs/kernel/overview.md) — Kernel internals (boot, memory, scheduler, signals)
- [userland dev guide](/docs/getting_started/userland_dev_guide.md) — Compiler flags, PIC model, ELF details
- [syscall.md](/docs/kernel/syscall.md) — Complete syscall reference (shared numbering)
