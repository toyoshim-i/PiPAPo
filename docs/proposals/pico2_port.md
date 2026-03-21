# Raspberry Pi Pico 2 Target Port Plan

Porting PPAP to the Raspberry Pi Pico 2 (RP2350, dual Cortex-M33).
The RP2350 is the successor to the RP2040 and shares many design
principles, making this the most natural next hardware target for PPAP.

## Status

| Phase | Description | Status |
|-------|-------------|--------|
| Pi2-1 | Target skeleton and boot | **Complete** |
| Pi2-2 | ARMv8-M MPU | **Complete** |
| Pi2-3 | Full test suite + SMP | **Complete** — 25 tests pass (extended suite) |
| Pi2-4 | Hardware FPU | **Complete** — softfp ABI, signal delivery, lazy stacking |
| Pi2-5 | PicoCalc 2 | Out of scope — see [arm_m.md](../targets/arm_m.md) §17 |
| Pi2-6 | PSRAM | Out of scope — see [arm_m.md](../targets/arm_m.md) §17 |
| Pi2-7 | TrustZone | Out of scope — see [arm_m.md](../targets/arm_m.md) §17 |

### Implementation Notes (vs. original plan)

- **User-space float ABI**: Uses `-mfloat-abi=softfp` (not `-mfloat-abi=hard`)
  so user binaries stay compatible with soft-float musl libc.  Hardware VFP
  instructions are still generated; only the calling convention stays soft.
- **Signal delivery with FPU**: Required per-core `svc_exc_return[]` tracking
  in `trap.S` to correctly handle extended (FPU) exception frames through
  signal setup and sigreturn.  Not anticipated in the original plan.
- **Separate shared build directories**: `build/arm_m` (M0+ soft-float) and
  `build/arm_m33` (M33 softfp) prevent hard-float binaries from clobbering
  soft-float ones when building multiple ARM targets.
- **FPB hardware breakpoints**: Disabled on M33 via `FPB_CTRL = 0` in
  `target_early_init()` — the FPB defaults to enabled on M33 and interferes
  with debugger-controlled breakpoints.
- **cmake/arm_m.cmake**: Required changes (contradicts §6 "no change needed"):
  `PPAP_ARM_HARDFLOAT` flag and conditional shared build directory.
- **test_pdb zombie fix**: pdb's quit/detach handlers now `waitpid()` after
  `kill()` to reap tracee children, preventing process table exhaustion in
  full suite runs.

---

## 1. RP2350 vs RP2040

| Feature | RP2040 (Pico 1) | RP2350 (Pico 2) |
|---------|-----------------|-----------------|
| CPU cores | 2x Cortex-M0+ (ARMv6-M) | 2x Cortex-M33 (ARMv8-M) **or** 2x Hazard3 (RISC-V) |
| Clock | 133 MHz | 150 MHz (up to ~300 MHz overclocked) |
| SRAM | 264 KB | **520 KB** (10 banks x 48 KB + 2 x 16 KB) |
| Flash | External QSPI (XIP) | External QSPI (XIP), up to 16 MB |
| PSRAM | Not supported | Optional external QSPI PSRAM (XIP, up to 16 MB) |
| ISA | Thumb-1 only | Thumb-1 + **Thumb-2** (32-bit instructions) |
| FPU | None | **Single-precision FPU** (hardware float) |
| DSP | None | **DSP extensions** (SIMD, saturating arithmetic) |
| MPU | 8 regions (ARMv6-M MPU) | **8 regions (ARMv8-M MPU)** + SAU |
| TrustZone | No | **Yes** (Secure/Non-Secure worlds) |
| Boot | ROM -> boot2 -> kernel | ROM -> boot2 -> kernel (compatible) |
| PIO | 2x PIO, 4 SM each | 3x PIO, 4 SM each |
| Peripherals | 2x SPI, 2x I2C, 2x UART | 2x SPI, 2x I2C, 2x UART (compatible) |
| DMA | 12 channels | 16 channels |
| USB | 1x USB 1.1 | 1x USB 1.1 (compatible) |
| Package | QFN-56 | QFN-60 (mostly pin-compatible) |
| ADC | 4 channels, 12-bit | 4 channels, 12-bit (compatible) |
| Spinlocks | 32 hardware spinlocks | 32 hardware spinlocks (compatible) |

### Key Takeaway

The RP2350 is an **evolutionary upgrade**, not a revolutionary change.
The peripheral set is nearly identical.  The biggest differences are:

1. **More SRAM** (520 KB vs 264 KB) -- ~130 pages vs ~51 pages
2. **Thumb-2 ISA** -- 32-bit Thumb instructions, smaller code
3. **Hardware FPU** -- single-precision float at near-zero cost
4. **ARMv8-M MPU** -- different register interface from ARMv6-M
5. **TrustZone** -- optional Secure/Non-Secure partitioning
6. **RISC-V option** -- the Hazard3 cores are selectable at boot

---

## 2. Goals and Scope

### 2.1 Primary Goal

Produce a bootable PPAP system on the Raspberry Pi Pico 2 that:

- Boots from QSPI flash (XIP) using the existing stage1 boot mechanism.
- Runs dual-core preemptive scheduling (same as pico1).
- Passes the full PPAP test suite (`runtests`).
- Takes advantage of the larger SRAM (520 KB -> ~130 user pages).
- Produces smaller code via Thumb-2 encoding.

### 2.2 Completed Extended Goal

- Hardware FPU enabled for user-space programs (softfp ABI).

### 2.3 Out of Scope

The following are documented as future work in [arm_m.md](../targets/arm_m.md) §17:

- PSRAM support (16 MB external QSPI PSRAM as extended page pool).
- PicoCalc 2 hardware target (blocked on hardware availability).
- TrustZone partitioning (kernel in Secure, user in Non-Secure).
- RISC-V core support (Hazard3, separate architecture port).
- Wi-Fi/Bluetooth (Pico 2 W uses CYW43439 -- complex driver).
- USB host mode.
- DMA-driven file I/O.

---

## 3. What Changes

### 3.1 Architecture Layer (ARMv8-M vs ARMv6-M)

The Cortex-M33 is source-compatible with Cortex-M0+ for most code, but
the assembly-level differences matter for PPAP's arch layer.

#### 3.1.1 Assembly Files: `.cpu` Directive and ISA Workarounds

All four assembly files under `src/arch/arm_m/` currently specify:

```asm
.cpu cortex-m0plus
```

On Cortex-M33, these must change to `.cpu cortex-m33`.  Rather than
duplicating files, we use a preprocessor define set by the build system:

```asm
#if defined(__ARM_ARCH) && __ARM_ARCH >= 8
.cpu cortex-m33
#else
.cpu cortex-m0plus
#endif
```

The actual code impact in each file:

| File | M0+ Limitation | M33 Opportunity | Change Required |
|------|---------------|-----------------|-----------------|
| `boot.S` | `.cpu` directive only | Same Reset_Handler logic works | `.cpu` directive only |
| `stage1.S` | `.cpu` directive only | Same VTOR-redirect logic | `.cpu` directive only |
| `switch.S` | r8-r11 via r4-r7 temps | Direct `stmdb`/`ldmia` on r4-r11 | `.cpu` directive + optional cleanup |
| `trap.S` | r8-r11 via r4-r7 temps | Same, but current code works fine | `.cpu` directive only |

**Key insight**: All M0+ assembly is a valid subset of M33 -- the port
works by changing only the `.cpu` directive.  The M0+ workarounds
(mov r4,r8; stmia r4-r7 pattern) execute correctly on M33, just slightly
less efficiently.  Optimization can be done later.

#### 3.1.2 Exception Entry/Return

Cortex-M33 extends the exception frame with optional FPU context:

| Feature | M0+ (ARMv6-M) | M33 (ARMv8-M) |
|---------|---------------|----------------|
| Auto-pushed frame | {r0-r3, r12, lr, pc, xpsr} (8 words) | Same basic frame + optional {s0-s15, fpscr} |
| EXC_RETURN | 0xFFFFFFF9 (MSP) / 0xFFFFFFFD (PSP) | Same + bit 4 controls FPU frame |
| Stack alignment | 8-byte | 8-byte |
| PendSV | Yes | Yes (identical mechanism) |
| SysTick | Yes | Yes (identical) |

**For the initial port (no FPU)**: The exception frame is identical.
EXC_RETURN bit 4 defaults to 1 (no FPU frame) when FPU is disabled,
so `switch.S` and `trap.S` work unchanged.

**When FPU is enabled (Phase Pi2-4)**: Lazy stacking means the hardware
only extends the frame when s0-s15 are actually dirty.  The PendSV
handler needs no code change -- the CPU handles it via FPCCR.LSPEN.

#### 3.1.3 Instruction Set

Thumb-2 adds 32-bit instructions to the Thumb ISA:

| Instruction | M0+ | M33 | PPAP impact |
|------------|-----|-----|-------------|
| `cbz/cbnz` (compare-branch) | No | Yes | Compiler uses automatically |
| `it` (if-then) | No | Yes | Compiler uses automatically |
| `movw/movt` (32-bit immediate) | No | Yes | Eliminates literal pools |
| `sdiv/udiv` | No | Yes | Hardware divide |
| `tbb/tbh` (table branch) | No | Yes | Efficient switch statements |
| `clz` (count leading zeros) | No | Yes | Useful for bitmap allocator |
| `mla/mls` (multiply-accumulate) | No | Yes | DSP |
| `ldrex/strex` (exclusive access) | No | Yes | Better atomics |
| `stmdb/ldmia` with high regs | No | Yes | Simpler context switch |

**Impact**: Recompile with `-mcpu=cortex-m33 -mthumb` produces smaller,
faster code with no C source changes.  Assembly files need only the
`.cpu` directive change (see 3.1.1).

#### 3.1.4 MPU (Memory Protection Unit)

The ARMv8-M MPU has a different register interface from ARMv6-M:

| Aspect | ARMv6-M MPU (RP2040) | ARMv8-M MPU (RP2350) |
|--------|---------------------|---------------------|
| Regions | 8 | 8 |
| Size granularity | Power-of-2 (32 B minimum) | **Any multiple of 32 B** |
| Size encoding | RASR.SIZE field (log2) | Base + Limit addresses |
| Sub-region disable | 8 sub-regions per region | Not needed (arbitrary size) |
| Registers | RNR + RBAR + RASR | RNR + RBAR + RLAR |
| Attributes | Inline in RASR (AP, XN, C, B) | Inline in RBAR (SH, AP, XN) + MAIR index in RLAR |
| Execute Never | RASR.XN bit 28 | RBAR.XN bit 0 |
| PRIVDEFENA | Yes | Yes (same concept) |

The ARMv8-M MPU is **simpler to use** for PPAP because region sizes don't
need to be powers of 2.  However, the register encoding is completely
different, requiring `#if` guards in `mpu.c`.

Current `mpu.c` code (ARMv6-M):
```c
/* Region setup: RASR encodes size as log2, attributes inline */
MPU_RNR  = region;
MPU_RBAR = base;
MPU_RASR = RASR_XN | RASR_AP(AP_RW_PRIV) | RASR_C | RASR_B |
           RASR_SIZE(13u) | RASR_ENABLE;   /* 16 KB */
```

New ARMv8-M code:
```c
/* Region setup: RBAR encodes AP/SH/XN, RLAR encodes limit + MAIR index */
MPU_RNR  = region;
MPU_RBAR = base | (AP_RW_PRIV << 1) | XN;
MPU_RLAR = ((base + size - 1) & ~0x1Fu) | (attr_idx << 1) | ENABLE;
/* Must also program MAIR0/MAIR1 for memory type attributes */
```

**Key difference**: ARMv8-M MPU uses MAIR (Memory Attribute Indirection
Register) -- attributes are defined in MAIR0/MAIR1 and referenced by
index from RLAR.  The current RASR-inline approach won't work.

### 3.2 Memory Layout

With 520 KB SRAM, the memory split improves significantly:

#### pico2 (standard Pico 2)

| Region | Address | Size | Use |
|--------|---------|------|-----|
| Kernel data | 0x20000000 | 24 KB | .data + .bss + kernel stack |
| Page pool | 0x20006000 | 464 KB (~116 pages) | User process pages |
| I/O buffer | 0x20078000 | 16 KB | Shared I/O (SD card, etc.) |
| DMA / Reserved | 0x2007C000 | 16 KB | DMA buffers, scratch |

Compared to pico1 (51 pages) and pico1calc (50 pages), **116 pages is a
2.3x improvement**.  This allows more processes, larger programs, and more
filesystem buffers.

**Note**: The pico2 bare target has no SD card or display, so I/O buffer
and DMA regions can be reclaimed for the page pool in a future revision
(giving ~124 pages).  The initial port keeps the same layout as pico1
for structural consistency.

#### pico2 with PSRAM (extended goal)

If external QSPI PSRAM is connected (Pico 2 supports up to 16 MB):

| Region | Address | Size | Use |
|--------|---------|------|-----|
| SRAM (kernel) | 0x20000000 | 40 KB | .data + .bss + kernel stack + hot data |
| SRAM (fast pool) | 0x2000A000 | 480 KB (~120 pages) | Hot user pages |
| PSRAM (XIP) | 0x11000000 | 16 MB (~4096 pages) | Cold user pages, file cache |

### 3.3 Boot Sequence

The RP2350 boot sequence is compatible with RP2040:

1. **Boot ROM** -- selects ARM or RISC-V cores, loads boot2 from flash
2. **boot2** (256 B) -- configures QSPI flash timing, sets VTOR
3. **stage1** (`src/arch/arm_m/stage1.S`) -- sets VTOR to kernel vector table
4. **Kernel** -- `Reset_Handler` -> `kmain()`

**SDK 2.2.0**: We already have Pico SDK 2.2.0 in `third_party/pico-sdk/`,
which supports both RP2040 and RP2350.  The SDK provides RP2350 boot2
variants under `src/rp2350/boot_stage2/` and selects the correct one via
`PICO_PLATFORM=rp2350-arm-s`.  No SDK upgrade needed.

**stage1.S**: The existing code is position-independent and reads
`__kernel_vtor` from the linker script.  It works unchanged on RP2350
as long as the linker script defines the correct `__kernel_vtor` address.
Only the `.cpu` directive needs updating (see 3.1.1).

**boot2 linkage**: The pico1 CMakeLists.txt currently links
`$<TARGET_OBJECTS:bs2_default_library>`.  The SDK automatically builds
the correct boot2 for the selected `PICO_PLATFORM`, so this line works
unchanged for RP2350.

### 3.4 Peripheral Compatibility

Most RP2350 peripherals are register-compatible with RP2040:

| Peripheral | Compatible? | Notes |
|-----------|------------|-------|
| UART (PL011) | **Yes** | Same registers |
| SPI (PL022) | **Yes** | Same registers |
| I2C (DW APB) | **Yes** | Same registers |
| GPIO / IO_BANK0 | **Yes** | Same registers (more pins on QFN-60) |
| PIO | **Yes** | Third PIO block added |
| ADC | **Yes** | Same registers |
| Timer | **Yes** | Same registers |
| SysTick | **Yes** | Standard Cortex-M |
| SIO (spinlocks) | **Yes** | Same 32 spinlocks, same SIO_CPUID |
| DMA | Mostly | 16 channels (was 12), same programming model |
| USB | **Yes** | Same USB 1.1 controller |
| XIP / QSPI | Extended | PSRAM support added |
| Clocks / PLL | **Different** | Different PLL structure (see 3.4.1) |
| Watchdog | **Yes** | Same registers |
| RESETS | **Yes** | Same registers |

**Impact**: Drivers `uart_rp2040.c`, `spi_rp2040.c`, `i2c_rp2040.c`,
`spi_lcd_rp2040.c` work unchanged.  Only `clock_rp2040.c` needs changes.

#### 3.4.1 Clock Configuration (PLL Differences)

The RP2350 PLL has the same register layout as RP2040 (CS, PWR, FBDIV_INT,
PRIM at the same offsets from PLL_SYS base 0x40028000), but the target
frequency and divider values differ:

| Parameter | RP2040 | RP2350 |
|-----------|--------|--------|
| XOSC | 12 MHz | 12 MHz (same on Pico 2 board) |
| Target clk_sys | 133 MHz | 150 MHz |
| VCO | 1596 MHz (FBDIV=133) | 1500 MHz (FBDIV=125) |
| POSTDIV1 x POSTDIV2 | 6 x 2 = 12 | 5 x 2 = 10 |
| Result | 1596/12 = 133 MHz | 1500/10 = 150 MHz |

**Approach**: Parameterize `clock_rp2040.c` with target-provided defines
(`PPAP_PLL_FBDIV`, `PPAP_PLL_PD1`, `PPAP_PLL_PD2`), or create a separate
`clock_rp2350.c` with the RP2350 values.  The former is cleaner since the
register interface is identical.

**SysTick reload**: Currently hardcoded as `(133000000u/100u-1u)` in
`cpu.h`.  Must become `(PPAP_SYS_CLK_HZ/100u-1u)` where each target
defines `PPAP_SYS_CLK_HZ`.  For RP2350: `(150000000u/100u-1u) = 1499999`.

**UART baud divisor**: `uart_reinit_133mhz()` recalculates baud divisors
for 133 MHz.  Needs to use `PPAP_SYS_CLK_HZ` instead.

### 3.5 Shared Header Updates

| Header | Change | Reason |
|--------|--------|--------|
| `cpu.h` | Guard name change: `PPAP_HW_CORTEX_M0PLUS_H` -> `PPAP_HW_CPU_H` | Applies to both M0+ and M33 |
| `cpu.h` | `SYSTICK_RELOAD` parameterized on `PPAP_SYS_CLK_HZ` | 133 MHz vs 150 MHz |
| `cpu.h` | Add CPACR, FPCCR, FPU registers (gated on `__ARM_ARCH >= 8`) | FPU support (Phase Pi2-4) |
| `rp2040.h` | No change needed | RP2350 peripherals at same addresses |

---

## 4. What Stays the Same

Everything above the arch layer is unchanged:

| Component | Status |
|-----------|--------|
| Kernel (C code) | Unchanged |
| VFS, all filesystems | Unchanged |
| Process model, scheduler | Unchanged |
| Syscall dispatch (C) | Unchanged |
| Signal delivery | Changed — EXC_RETURN tracking for FPU frames |
| Pipe, FD, TTY | Unchanged |
| Trace / ptrace | Unchanged |
| All user-space programs | Recompile only (Thumb-2 automatic) |
| romfs / UFS images | Binary compatible (ARM ELF, re-linked for M33) |
| SPI SD card driver | Unchanged |
| Dual-core support | Unchanged (same SIO spinlock mechanism) |

---

## 5. New Opportunities (Extended Goals)

### 5.1 Hardware FPU

The Cortex-M33 has a single-precision VFPv5 FPU.  This enables:

- `float` operations at hardware speed (previously soft-float library)
- User-space programs compiled with `-mfloat-abi=hard -mfpu=fpv5-sp-d16`
- Lazy FPU context save on context switch (CONTROL.FPCA + EXC_RETURN bit 4)

**Implementation**: Enable FPU in `target_early_init()` via CPACR,
configure lazy stacking via FPCCR.  With lazy stacking enabled, the
hardware automatically saves/restores s0-s15 only when a process has
touched the FPU.  No changes to `switch.S` or `trap.S` needed.

### 5.2 TrustZone (ARMv8-M Security Extension)

TrustZone partitions the system into Secure and Non-Secure worlds.
This is an extended goal.  The initial port runs everything in the
Secure world (identical to RP2040 ignoring TrustZone).

### 5.3 PSRAM as Extended Page Pool

With 16 MB PSRAM, the page pool grows to ~4200 pages.  This is an
extended goal requiring XIP controller configuration and zone-based
page allocation.

### 5.4 RISC-V (Hazard3)

The RP2350 can boot in RISC-V mode (RV32IMAC).  This is a completely
new architecture -- comparable effort to the original m68k port.
Deferred to a separate proposal.

---

## 6. Files to Create and Modify

### New files

```
src/target/pico2/
  CMakeLists.txt        -- Build rules (PICO_PLATFORM=rp2350-arm-s)
  pico2.ld              -- Linker script (520 KB SRAM layout)
  pico2.h               -- Pin definitions, clock constants
  target_pico2.c        -- Target hooks (early_init, late_init)
  romfs/                -- Romfs overlay (same structure as pico1)
```

### Existing files to modify

| File | Change | Phase |
|------|--------|-------|
| `src/arch/arm_m/boot.S` | `.cpu` directive: M0+ or M33 via `#if __ARM_ARCH` | Pi2-1 |
| `src/arch/arm_m/stage1.S` | `.cpu` directive (same pattern) | Pi2-1 |
| `src/arch/arm_m/switch.S` | `.cpu` directive (same pattern) | Pi2-1 |
| `src/arch/arm_m/trap.S` | `.cpu` directive (same pattern) | Pi2-1 |
| `src/arch/arm_m/cpu.h` | Parameterize `SYSTICK_RELOAD` on `PPAP_SYS_CLK_HZ` | Pi2-1 |
| `src/drivers/arch/arm_m/clock_rp2040.c` | Parameterize PLL dividers on target defines | Pi2-1 |
| `src/drivers/arch/arm_m/uart_rp2040.c` | Parameterize baud divisor on `PPAP_SYS_CLK_HZ` | Pi2-1 |
| `src/kernel/mm/mpu.c` | Add `#if __ARM_ARCH >= 8` path for ARMv8-M MPU + MAIR | Pi2-2 |
| `scripts/run.sh` | Add `pico2` target | Pi2-1 |

### Files that need NO changes

| File | Reason |
|------|--------|
| `src/target/rp2040.h` | RP2350 peripheral addresses are the same |
| `src/drivers/arch/arm_m/spi_rp2040.c` | Register-compatible |
| `src/drivers/arch/arm_m/i2c_rp2040.c` | Register-compatible |
| `src/drivers/spi_sd.c` | No hardware dependency |
| All kernel C code | Architecture-independent |
| `cmake/arm_m.cmake` | Changed — `PPAP_ARM_HARDFLOAT` + split shared build dirs |

---

## 7. Implementation Phases (Detailed)

### Phase Pi2-1: Target Skeleton and Boot ✓

**Goal**: "PiPAPo booting... [pico2]" on UART at 115200 bps.

**Prerequisites**: Raspberry Pi Pico 2 board, picoprobe or SWD debugger.

#### Step 1a: Parameterize clock-dependent code

Before creating the new target, refactor shared code so the same sources
work for both 133 MHz (RP2040) and 150 MHz (RP2350):

1. **`cpu.h`**: Replace hardcoded `SYSTICK_RELOAD`:
   ```c
   /* Before: */
   #define SYSTICK_RELOAD  (133000000u/100u - 1u)
   /* After: */
   #ifndef PPAP_SYS_CLK_HZ
   #define PPAP_SYS_CLK_HZ 133000000u   /* default: RP2040 */
   #endif
   #define SYSTICK_RELOAD  (PPAP_SYS_CLK_HZ/100u - 1u)
   ```

2. **`clock_rp2040.c`**: Parameterize PLL dividers:
   ```c
   #ifndef PPAP_PLL_FBDIV
   #define PPAP_PLL_FBDIV  133   /* RP2040: 12 * 133 = 1596 MHz VCO */
   #define PPAP_PLL_PD1    6
   #define PPAP_PLL_PD2    2     /* 1596 / 12 = 133 MHz */
   #endif
   ```

3. **`uart_rp2040.c`**: Replace `uart_reinit_133mhz()` with
   `uart_reinit(uint32_t sys_clk_hz)`, or use `PPAP_SYS_CLK_HZ`.

4. **Assembly files**: Add conditional `.cpu` directive to all four
   `.S` files under `src/arch/arm_m/`.

5. **Verify**: Rebuild and test `pico1` and `qemu_arm` to confirm
   no regressions from parameterization.

**Deliverables**: All pico1/qemu_arm tests still pass.

#### Step 1b: Create pico2 target directory

1. `mkdir -p src/target/pico2/romfs`

2. **`pico2.h`** -- target constants:
   ```c
   #define PPAP_SYS_CLK_HZ    150000000u
   #define PPAP_PLL_FBDIV      125
   #define PPAP_PLL_PD1        5
   #define PPAP_PLL_PD2        2
   /* GPIO pins: same as Pico 1 (UART0 TX=0, RX=1) */
   #define UART_TX_PIN  0
   #define UART_RX_PIN  1
   ```

3. **`target_pico2.c`** -- copy from `target_pico1.c`, update:
   - `#include "pico2.h"` instead of `pico1.h`
   - `target_name()` returns `"pico2"`
   - `target_early_init()`: same UART + PLL init sequence

4. **`pico2.ld`** -- copy from `pico1.ld` (not pico1calc), update:
   ```
   RAM_KERNEL  (rwx) : ORIGIN = 0x20000000, LENGTH = 24K
   RAM_PAGES   (rwx) : ORIGIN = 0x20006000, LENGTH = 464K
   RAM_IOBUF   (rw)  : ORIGIN = 0x20078000, LENGTH = 16K
   RAM_DMA     (rw)  : ORIGIN = 0x2007C000, LENGTH = 16K
   ```
   (FLASH regions stay the same -- XIP base is 0x10000000 on both chips.)

5. **`CMakeLists.txt`** -- based on `pico1/CMakeLists.txt`:
   ```cmake
   set(PICO_PLATFORM rp2350-arm-s)
   set(PICO_BOARD pico2)
   # ... (must be set BEFORE pico_sdk_init())

   target_compile_definitions(ppap_pico2 PRIVATE
       PPAP_SYS_CLK_HZ=150000000u
       PPAP_PLL_FBDIV=125
       PPAP_PLL_PD1=5
       PPAP_PLL_PD2=2
       PAGE_POOL_BASE=0x20006000u
       PAGE_COUNT_MAX=116u
   )
   ```

6. **`scripts/run.sh`**: Add `pico2` case (build + OpenOCD flash).

**Deliverables**: `./scripts/run.sh --build pico2` produces a `.uf2`
and `.elf`.  Flashing to Pico 2 prints kernel banner on UART.

#### Step 1c: Smoke test

1. Flash via picoprobe: `openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg`
2. Or drag-and-drop `.uf2` via BOOTSEL button.
3. Connect UART (115200 8N1, GPIO 0/1).
4. Verify: `PiPAPo booting...` followed by `System clock: 150 MHz`.

**Potential issues**:
- Boot2 may need a different flash chip config -- check if
  `bs2_default_library` works on Pico 2's W25Q128JVS flash.
  (The SDK's `compile_time_choice.S` auto-detects, so this should work.)
- UART timing: if 150 MHz baud divisor is off, garbage on UART.
  Debug with SWD + `gdb-multiarch` first.

---

### Phase Pi2-2: MPU Update ✓

**Goal**: Memory protection works with ARMv8-M MPU.

#### Step 2a: Understand the ARMv8-M MPU register model

The key difference is the MAIR (Memory Attribute Indirection Register):

```
ARMv6-M: attributes encoded directly in RASR (AP, C, B, XN bits)
ARMv8-M: attributes defined in MAIR0/MAIR1, referenced by 3-bit index in RLAR
```

MAIR encoding for PPAP's 3 memory types:

| Index | Type | MAIR byte | Encoding |
|-------|------|-----------|----------|
| 0 | Normal, outer+inner write-back | 0xFF | Outer WB/WA, Inner WB/WA |
| 1 | Device-nGnRnE | 0x00 | Device memory (peripherals) |
| 2 | Normal, outer+inner write-through | 0xAA | Outer WT/WA, Inner WT/WA |

#### Step 2b: Implement ARMv8-M MPU path in `mpu.c`

Add `#if __ARM_ARCH >= 8` block:

```c
#if __ARM_ARCH >= 8
/* ARMv8-M MPU registers */
#define MPU_MAIR0   (*(volatile uint32_t *)0xE000EDC0u)
#define MPU_MAIR1   (*(volatile uint32_t *)0xE000EDC4u)
#define MPU_RLAR    (*(volatile uint32_t *)0xE000EDA0u)  /* replaces RASR */

/* RBAR: base[31:5] | SH[4:3] | AP[2:1] | XN[0] */
/* RLAR: limit[31:5] | (reserved)[4:3] | AttrIdx[3:1] | EN[0] */
#endif
```

Region map (same 4-region layout, different encoding):

| Region | Base | Limit | AP | XN | MAIR idx |
|--------|------|-------|----|----|----------|
| 0: Kernel data | 0x20000000 | 0x20005FFF | Priv RW (01) | Yes | 0 (WB) |
| 1: Flash XIP | 0x10000000 | 0x10FFFFFF | Priv+User RO (11) | No | 2 (WT) |
| 2: Process stack | per-process | +4KB | Priv+User RW (01)* | Yes | 0 (WB) |
| 3: Peripherals | 0x40000000 | 0x5FFFFFFF | Priv RW (01) | Yes | 1 (Device) |

*Note: ARMv8-M AP encoding differs: 00=Priv RW, 01=RW all, 10=Priv RO, 11=RO all.

#### Step 2c: Test

1. Build with MPU enabled.
2. Run a user process that attempts to read 0x20000000 (kernel memory).
3. Verify HardFault is triggered (not silent access).
4. Run runtests to confirm no false MPU faults.

**Deliverables**: MPU regions active, violation triggers HardFault.

---

### Phase Pi2-3: Full Test Suite ✓

**Goal**: All tests pass on Pico 2.

#### Step 3a: Verify dual-core

- SIO_CPUID at 0xD0000000 works identically on RP2350.
- `core_id_reg` indirect pointer mechanism in `switch.S` and `trap.S`
  works unchanged.
- Core 1 launch via SIO FIFO is the same.

#### Step 3b: Run runtests

1. Build with `PPAP_TESTS=ON`:
   ```
   cmake -S src/target/pico2 -B build/pico2 -DPPAP_TESTS=ON
   ```
2. Flash and observe UART output.
3. Expected: 17 tests pass (same as pico1 ARM subset -- h68k_dos and
   x68k tests are m68k-only).

#### Step 3c: Performance comparison

Compare with pico1 output (if available):
- Kernel text size (should be ~25% smaller due to Thumb-2)
- Boot time (should be similar or faster due to 150 MHz)

**Deliverables**: "ALL TESTS PASSED" on UART.

---

### Phase Pi2-4: FPU Support ✓

**Goal**: User-space programs can use hardware float.

**Status**: Complete.

#### Step 4a: Enable FPU ✓

FPU enabled in `target_early_init()` via CPACR and FPCCR:

```c
/* Enable CP10 and CP11 (FPU) in CPACR */
CPACR |= (0xF << 20);  /* Full access to CP10, CP11 */
/* Enable lazy stacking */
FPCCR |= (1u << 31) | (1u << 30);  /* ASPEN + LSPEN */
```

#### Step 4b: Compiler flags ✓

User-space uses **softfp ABI** (not hard-float as originally planned):
```cmake
set(PPAP_ARM_HARDFLOAT ON)  # in pico2/CMakeLists.txt
# cmake/user.cmake selects: -mfloat-abi=softfp -mfpu=fpv5-sp-d16
```

Softfp generates hardware VFP instructions but passes float args in
integer registers — compatible with soft-float musl libc without
rebuilding musl.  Kernel compiles with hard-float.

#### Step 4c: Context switch ✓

Lazy stacking handles s0-s15 transparently.  s16-s31 are callee-saved
and preserved across function calls, so `switch.S` needs no FPU code.

**Key discovery**: Signal delivery required EXC_RETURN tracking.  When
FPU is active, the SVC exception frame is extended (26 words vs 8).
`signal_setup_frame` must account for this:

- `svc_exc_return[core_id]` saved by `trap.S` on SVC entry
- `signal_setup_frame` saves original EXC_RETURN, forces basic frame
- `sys_sigreturn` restores original EXC_RETURN

#### Step 4d: Float tests ✓

Two tests added:
- `test_float`: basic hardware float arithmetic (3 assertions)
- `test_signal_float`: float registers survive signal delivery where
  the handler itself uses FPU (8 assertions)

**Deliverables**: Both tests pass on Pico 2 and QEMU ARM.

---

### Phase Pi2-5: PicoCalc 2 (if applicable)

**Goal**: PPAP runs on PicoCalc hardware with RP2350.

1. Create `src/target/pico2calc/` (LCD + keyboard drivers from pico1calc).
2. Update SPI/I2C pin assignments for PicoCalc 2 board (if different).
3. Verify LCD display and keyboard input.
4. Include SD card, block device, VFS support.

**Deliverables**: Interactive shell on PicoCalc 2 hardware.

---

### Phase Pi2-6: PSRAM Support (Extended)

**Goal**: 16 MB PSRAM available as extended page pool.

1. Configure XIP controller for PSRAM chip select.
2. Add PSRAM zone to page allocator (separate from SRAM pages).
3. Implement zone preference: SRAM for hot pages, PSRAM for cold pages.
4. Test with many concurrent processes to exercise PSRAM pages.

**Deliverables**: `runtests` passes with >100 pages allocated from PSRAM.

---

### Phase Pi2-7: TrustZone (Extended)

**Goal**: Kernel runs in Secure world, user processes in Non-Secure.

1. Configure SAU: kernel memory -> Secure, user memory -> Non-Secure.
2. Syscall entry via `SG` (Secure Gateway) instruction.
3. Non-Secure process faults on kernel memory access.
4. Verify all tests still pass with TrustZone partitioning.

**Deliverables**: User process attempting kernel memory read triggers
SecureFault, not silent access.

---

## 8. Code Size Comparison (Expected)

Thumb-2 produces significantly smaller code than Thumb-1 for the same
C source.  Expected savings:

| Component | Pico 1 (Thumb-1) | Pico 2 (Thumb-2) | Savings |
|-----------|-----------------|-----------------|---------|
| Kernel text | ~80 KB | ~60 KB | ~25% |
| User programs | ~20 KB each | ~15 KB each | ~25% |
| romfs total | ~200 KB | ~150 KB | ~25% |

The savings come from:
- 32-bit Thumb-2 instructions eliminate many multi-instruction sequences
- `movw`/`movt` replaces literal pool loads
- `cbz`/`cbnz` replaces `cmp` + `b` pairs
- Hardware `sdiv`/`udiv` replaces library calls (~200 bytes each)
- `it` blocks replace branch-around patterns

---

## 9. Risks and Mitigations

### 9.1 Pico SDK Compatibility (RESOLVED)

~~The Pico 2 requires Pico SDK 2.0+.  PPAP currently uses Pico SDK 1.x.~~

**Resolved**: We already have Pico SDK 2.2.0 in `third_party/pico-sdk/`.
The existing pico1 and pico1calc targets already build against SDK 2.x.
No SDK upgrade needed.

### 9.2 Flash Boot2

The RP2350 boot ROM and boot2 have a different internal structure from
RP2040, but the SDK abstracts this.  The `bs2_default_library` CMake
target produces the correct boot2 for the selected `PICO_PLATFORM`.

**Risk**: The Pico 2's flash chip may need a different boot2 variant.
The SDK's `compile_time_choice.S` auto-probes the flash chip, so this
should work out of the box.

**Mitigation**: If boot2 fails, the Pico 2 won't boot at all -- easy to
diagnose.  Fall back to a known-working boot2 from the SDK examples.

### 9.3 Clock PLL Configuration

The RP2350 PLL uses the same register interface but different target
values.  A wrong VCO frequency (outside 400-1600 MHz range) will fail
to lock.

**Mitigation**: VCO = 12 * 125 = 1500 MHz, well within range.  The SDK
examples use these same values.

### 9.4 Assembly Compatibility

M0+ Thumb-1 assembly is a subset of M33 Thumb-2, so existing assembly
works without changes (beyond `.cpu` directive).  However, the assembler
may emit warnings about suboptimal instruction sequences.

**Mitigation**: Phase Pi2-1 changes only the `.cpu` directive.  Assembly
optimization (using `stmdb`/`ldmia` with high registers) is deferred.

### 9.5 PSRAM Latency

PSRAM access is significantly slower than SRAM (~5-10x for random access).

**Mitigation**: Zone-based allocation with SRAM preference.  Deferred to
Phase Pi2-6.

### 9.6 TrustZone Complexity

TrustZone adds complexity to the syscall path and exception handling.

**Mitigation**: Deferred to Phase Pi2-7.  Initial port runs entirely
in Secure world.

---

## 10. Dependency Graph

```
Pi2-1a (parameterize shared code)
  |
  v
Pi2-1b (create pico2 target)
  |
  v
Pi2-1c (smoke test on hardware)
  |
  v
Pi2-2 (ARMv8-M MPU)
  |
  v
Pi2-3 (full test suite)
  |
  +---> Pi2-4 (FPU)
  |
  +---> Pi2-5 (PicoCalc 2)
  |
  +---> Pi2-6 (PSRAM)
          |
          v
        Pi2-7 (TrustZone)
```

Pi2-1 through Pi2-3 are the core port.  Everything after Pi2-3 is an
independent extension.

---

## 11. Related Documentation

- [docs/kernel/overview.md](../kernel/overview.md) -- PPAP kernel architecture
- [docs/reference/picocalc.md](../reference/picocalc.md) -- PicoCalc hardware reference
- [docs/proposals/pizero_port.md](pizero_port.md) -- Pi Zero port (ARM1176, MMU -- different scope)
