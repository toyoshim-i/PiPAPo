# Raspberry Pi Pico 2 Target Port Plan

Porting PPAP to the Raspberry Pi Pico 2 (RP2350, dual Cortex-M33).
The RP2350 is the successor to the RP2040 and shares many design
principles, making this the most natural next hardware target for PPAP.

---

## 1. RP2350 vs RP2040

| Feature | RP2040 (Pico 1) | RP2350 (Pico 2) |
|---------|-----------------|-----------------|
| CPU cores | 2× Cortex-M0+ (ARMv6-M) | 2× Cortex-M33 (ARMv8-M) **or** 2× Hazard3 (RISC-V) |
| Clock | 133 MHz | 150 MHz (up to ~300 MHz overclocked) |
| SRAM | 264 KB | **520 KB** (10 banks × 48 KB + 2 × 16 KB) |
| Flash | External QSPI (XIP) | External QSPI (XIP), up to 16 MB |
| PSRAM | Not supported | Optional external QSPI PSRAM (XIP, up to 16 MB) |
| ISA | Thumb-1 only | Thumb-1 + **Thumb-2** (32-bit instructions) |
| FPU | None | **Single-precision FPU** (hardware float) |
| DSP | None | **DSP extensions** (SIMD, saturating arithmetic) |
| MPU | 8 regions (ARMv6-M MPU) | **8 regions (ARMv8-M MPU)** + SAU |
| TrustZone | No | **Yes** (Secure/Non-Secure worlds) |
| Boot | ROM → boot2 → kernel | ROM → boot2 → kernel (compatible) |
| PIO | 2× PIO, 4 SM each | 3× PIO, 4 SM each |
| Peripherals | 2× SPI, 2× I2C, 2× UART | 2× SPI, 2× I2C, 2× UART (compatible) |
| DMA | 12 channels | 16 channels |
| USB | 1× USB 1.1 | 1× USB 1.1 (compatible) |
| Package | QFN-56 | QFN-60 (mostly pin-compatible) |
| ADC | 4 channels, 12-bit | 4 channels, 12-bit (compatible) |
| Spinlocks | 32 hardware spinlocks | 32 hardware spinlocks (compatible) |

### Key Takeaway

The RP2350 is an **evolutionary upgrade**, not a revolutionary change.
The peripheral set is nearly identical.  The biggest differences are:

1. **More SRAM** (520 KB vs 264 KB) — ~130 pages vs ~51 pages
2. **Thumb-2 ISA** — 32-bit Thumb instructions, smaller code
3. **Hardware FPU** — single-precision float at near-zero cost
4. **ARMv8-M MPU** — slightly different register interface from ARMv6-M
5. **TrustZone** — optional Secure/Non-Secure partitioning
6. **RISC-V option** — the Hazard3 cores are selectable at boot

---

## 2. Goals and Scope

### 2.1 Primary Goal

Produce a bootable PPAP system on the Raspberry Pi Pico 2 that:

- Boots from QSPI flash (XIP) using the existing stage1 boot mechanism.
- Runs dual-core preemptive scheduling (same as pico1).
- Passes the full PPAP test suite (`runtests`).
- Takes advantage of the larger SRAM (520 KB → ~130 user pages).
- Produces smaller code via Thumb-2 encoding.

### 2.2 Extended Goals

- PSRAM support for dramatically larger page pool (~4000+ pages from 16 MB).
- Hardware FPU enabled for user-space programs.
- TrustZone partitioning (kernel in Secure world, user in Non-Secure).
- RISC-V core support (Hazard3, a completely new architecture for PPAP).
- PicoCalc 2 hardware target (if/when it ships with RP2350).

### 2.3 Out of Scope

- Wi-Fi/Bluetooth (Pico 2 W uses CYW43439, same as Pico W — complex
  driver, not needed for initial port).
- USB host mode.
- DMA-driven file I/O.

---

## 3. What Changes

### 3.1 Architecture Layer (ARMv8-M vs ARMv6-M)

The Cortex-M33 is source-compatible with Cortex-M0+ for most code, but
the assembly-level differences matter for PPAP's arch layer.

#### Exception Entry/Return

Cortex-M33 extends the exception frame with optional FPU context:

| Feature | M0+ (ARMv6-M) | M33 (ARMv8-M) |
|---------|---------------|----------------|
| Auto-pushed frame | {r0-r3, r12, lr, pc, xpsr} (8 words) | Same basic frame + optional {s0-s15, fpscr} |
| EXC_RETURN | 0xFFFFFFF9 (MSP) / 0xFFFFFFFD (PSP) | Same + bit 4 controls FPU frame |
| Stack alignment | 8-byte | 8-byte |
| PendSV | Yes | Yes (identical mechanism) |
| SysTick | Yes | Yes (identical) |

**Impact**: `switch.S` and `trap.S` need minor changes for FPU frame
handling.  The basic PendSV context switch mechanism is identical.

#### Instruction Set

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

**Impact**: Recompile with `-mcpu=cortex-m33 -mthumb` produces smaller,
faster code with no source changes.  The only arch-level asm that needs
updating is `switch.S`, `trap.S`, and `boot.S`.

#### MPU (Memory Protection Unit)

The ARMv8-M MPU has a different register interface from ARMv6-M:

| Aspect | ARMv6-M MPU (RP2040) | ARMv8-M MPU (RP2350) |
|--------|---------------------|---------------------|
| Regions | 8 | 8 |
| Size granularity | Power-of-2 (32 B minimum) | **Any multiple of 32 B** |
| Size encoding | RASR.SIZE field (log2) | Base + Limit addresses |
| Sub-region disable | 8 sub-regions per region | Not needed (arbitrary size) |
| Registers | RBAR + RASR per region | RBAR + RLAR per region |
| Execute Never | RASR.XN bit | RLAR.XN bit |
| PRIVDEFENA | Yes | Yes (same concept) |

The ARMv8-M MPU is **simpler to use** because region sizes don't need to
be powers of 2.  This is a strict improvement for PPAP — the current MPU
setup code in `mpu.c` can be simplified.

```c
/* RP2040 (ARMv6-M): region must be power-of-2 aligned */
MPU->RBAR = base | (region << 0) | (1 << 4);  /* VALID + REGION */
MPU->RASR = (size_log2 << 1) | attrs | 1;      /* SIZE + ATTRS + ENABLE */

/* RP2350 (ARMv8-M): base and limit are arbitrary (32-byte granularity) */
MPU->RNR  = region;
MPU->RBAR = base | (sh << 3) | (ap << 1) | xn;
MPU->RLAR = (limit & ~0x1F) | attrs_idx << 1 | 1;  /* LIMIT + ATTR + ENABLE */
```

**Impact**: Rewrite `mpu.c` (small file, ~80 lines).  The new MPU is
easier to configure correctly.

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
2.3× improvement**.  This allows more processes, larger programs, and more
filesystem buffers.

#### pico2 with PSRAM

If external QSPI PSRAM is connected (Pico 2 supports up to 16 MB):

| Region | Address | Size | Use |
|--------|---------|------|-----|
| SRAM (kernel) | 0x20000000 | 40 KB | .data + .bss + kernel stack + hot data |
| SRAM (fast pool) | 0x2000A000 | 480 KB (~120 pages) | Hot user pages |
| PSRAM (XIP) | 0x11000000 | 16 MB (~4096 pages) | Cold user pages, file cache |

With PSRAM, the page pool grows to **~4200 pages** — comparable to a small
desktop system.  PSRAM is accessed via XIP (execute-in-place), so it can
hold both code and data, though at higher latency than SRAM.

### 3.3 Boot Sequence

The RP2350 boot sequence is compatible with RP2040:

1. **Boot ROM** — selects ARM or RISC-V cores, loads boot2 from flash
2. **boot2** (256 B) — configures QSPI flash timing, sets VTOR
3. **stage1** (`src/boot/stage1.S`) — sets VTOR to kernel vector table
4. **Kernel** — `Reset_Handler` → `kmain()`

The Pico SDK handles boot2.  PPAP's stage1 needs minor updates:
- VTOR address may differ if flash layout changes
- RP2350 boot ROM has additional security features (signed boot) that
  can be ignored for development

### 3.4 Peripheral Compatibility

Most RP2350 peripherals are register-compatible with RP2040:

| Peripheral | Compatible? | Notes |
|-----------|------------|-------|
| UART (PL011) | **Yes** | Same registers |
| SPI | **Yes** | Same registers |
| I2C | **Yes** | Same registers |
| GPIO / IO_BANK0 | **Yes** | Same registers (more pins on QFN-60) |
| PIO | **Yes** | Third PIO block added |
| ADC | **Yes** | Same registers |
| Timer | **Yes** | Same registers |
| SysTick | **Yes** | Standard Cortex-M |
| SIO (spinlocks) | **Yes** | Same 32 spinlocks |
| DMA | Mostly | 16 channels (was 12), same programming model |
| USB | **Yes** | Same USB 1.1 controller |
| XIP / QSPI | Extended | PSRAM support added |
| Clocks / PLL | Similar | Different clock tree, same concepts |
| Watchdog | **Yes** | Same registers |
| RESETS | **Yes** | Same registers |

**Impact**: Most drivers (`uart.c`, `clock.c`, SPI, I2C) work unchanged.
Only `clock.c` may need PLL frequency adjustments for the different
default crystal and target clock.

---

## 4. What Stays the Same

Everything above the arch layer is unchanged:

| Component | Status |
|-----------|--------|
| Kernel (C code) | Unchanged |
| VFS, all filesystems | Unchanged |
| Process model, scheduler | Unchanged |
| Syscall dispatch | Unchanged |
| Signal delivery | Unchanged |
| Pipe, FD, TTY | Unchanged |
| Trace / ptrace | Unchanged |
| All user-space programs | Recompile only (Thumb-2 automatic) |
| romfs / UFS images | Binary compatible |
| SPI SD card driver | Unchanged |
| Dual-core support | Unchanged (same SIO spinlock mechanism) |

---

## 5. New Opportunities

### 5.1 Hardware FPU

The Cortex-M33 has a single-precision VFPv5 FPU.  This enables:

- `float` operations at hardware speed (previously soft-float library)
- User-space programs compiled with `-mfloat-abi=hard -mfpu=fpv5-sp-d16`
- Lazy FPU context save on context switch (CONTROL.FPCA + EXC_RETURN bit 4)

**Implementation**: Enable FPU in `SystemInit`, configure lazy stacking
via FPCCR.  The PendSV handler already handles FPU context via EXC_RETURN
bit 4 on ARMv8-M — just needs to be aware of the extended frame size.

### 5.2 TrustZone (ARMv8-M Security Extension)

TrustZone partitions the system into Secure and Non-Secure worlds:

| World | Access | PPAP use |
|-------|--------|----------|
| Secure | Full hardware access | Kernel |
| Non-Secure | Restricted by SAU/IDAU | User processes |

This is more powerful than the MPU alone:

- **Secure** code cannot be read or executed by Non-Secure code
- Transitions via `SG` (Secure Gateway) instructions at defined entry points
- The SAU (Security Attribution Unit) defines memory region security
- Non-Secure code faults immediately on any Secure memory access

**Potential PPAP use**: Run the kernel in Secure world, user processes in
Non-Secure world.  Syscalls use the `SG` instruction.  This provides
hardware-enforced kernel isolation — a user process cannot corrupt the
kernel even with an MPU misconfiguration.

This is an extended goal.  The initial port runs everything in the
Secure world (same as RP2040 ignoring TrustZone).

### 5.3 PSRAM as Extended Page Pool

With 16 MB PSRAM:

- Page allocator maintains two zones: SRAM (fast) and PSRAM (slow)
- Hot pages (current process stacks, kernel data) in SRAM
- Cold pages (inactive process code, file cache) in PSRAM
- Page migration between zones when a process is scheduled/descheduled

The XIP controller handles PSRAM transparently — code and data in PSRAM
are accessed via normal memory reads at the PSRAM address range.  No
special caching code needed; the XIP cache handles it.

### 5.4 RISC-V (Hazard3)

The RP2350 can boot in RISC-V mode using the dual Hazard3 cores
(RV32IMAC — integer, multiply, atomic, compressed).  This would be a
**completely new architecture** for PPAP:

- New `src/arch/rv32/` directory (boot.S, switch.S, trap.S)
- New `ecall` syscall convention
- RISC-V compressed instructions (analogous to Thumb)
- RISC-V PMP (Physical Memory Protection) instead of ARM MPU

This is a significant effort and is deferred to a future proposal.  The
initial pico2 port targets ARM (Cortex-M33) only.

---

## 6. New Files

```
src/target/pico2/
  CMakeLists.txt        — Build rules (uses Pico SDK with RP2350)
  pico2.ld              — Linker script (520 KB SRAM layout)
  target_pico2.c        — Target hooks (early_init, late_init)
  romfs/                — Romfs overlay (same structure as pico1)
```

Existing files that need modification:

| File | Change |
|------|--------|
| `src/arch/arm_m/mpu.c` | Add `#if` for ARMv8-M MPU register interface |
| `src/arch/arm_m/switch.S` | Add FPU lazy stacking awareness |
| `src/boot/stage1.S` | Verify VTOR address for RP2350 flash layout |
| `cmake/arm_m.cmake` | Add `-mcpu=cortex-m33` variant |
| `scripts/build.sh` | Add `pico2` target |
| `scripts/run.sh` | Add `pico2` target (OpenOCD + picoprobe) |

### Target-specific configuration

```cmake
# src/target/pico2/CMakeLists.txt
set(PICO_PLATFORM rp2350-arm-s)   # RP2350, ARM, Secure world
set(PPAP_PAGE_COUNT_MAX 116)       # 464 KB / 4 KB
set(PPAP_KERNEL_DATA_SIZE 24576)   # 24 KB
set(PPAP_IO_BUFFER_SIZE  16384)    # 16 KB
```

---

## 7. Implementation Phases

### Phase Pi2-1: Target Skeleton and Boot

**Goal**: "PiPAPo booting... [pico2]" on UART.

1. Create `src/target/pico2/` by copying `pico1/` as a starting point.
2. Update `CMakeLists.txt` for RP2350 (`PICO_PLATFORM=rp2350-arm-s`).
3. Update linker script for 520 KB SRAM layout.
4. Update compiler flags: `-mcpu=cortex-m33 -mfpu=fpv5-sp-d16 -mfloat-abi=hard`.
5. Build and flash via picoprobe or UF2.

**Verification**: Kernel banner on UART at 115200 bps.

### Phase Pi2-2: MPU Update

**Goal**: Memory protection works with ARMv8-M MPU.

1. Add ARMv8-M MPU configuration to `mpu.c` (conditional on `__ARM_ARCH >= 8`).
2. Configure regions: kernel RW, flash RO+XN, user RW, peripherals.
3. Verify user process faults on kernel memory access.

**Verification**: MPU violation triggers HardFault (not silent corruption).

### Phase Pi2-3: Dual-Core and Integration Tests

**Goal**: Full test suite passes on Pico 2.

1. Verify dual-core boot (same SIO spinlock mechanism as RP2040).
2. Verify SysTick on both cores.
3. Run `runtests`.
4. Compare test results with pico1 (should be identical + faster).

**Verification**: "ALL TESTS PASSED" on UART, both cores active.

### Phase Pi2-4: FPU Support

**Goal**: User-space programs can use hardware float.

1. Enable FPU in `SystemInit` (CPACR register).
2. Configure lazy stacking (FPCCR.ASPEN = 1, FPCCR.LSPEN = 1).
3. Verify context switch preserves FPU state correctly.
4. Add a float test (`test_float`) that verifies hardware FP results.

**Verification**: `test_float` passes; FPU context preserved across
context switches and syscalls.

### Phase Pi2-5: PicoCalc 2 (if applicable)

**Goal**: PPAP runs on PicoCalc hardware with RP2350.

1. Create `src/target/pico2calc/` (LCD + keyboard drivers from pico1calc).
2. Update SPI/I2C pin assignments for PicoCalc 2 board.
3. Verify LCD display and keyboard input.

**Verification**: Interactive shell on PicoCalc 2 hardware.

### Phase Pi2-6: PSRAM Support (Extended)

**Goal**: 16 MB PSRAM available as extended page pool.

1. Configure XIP controller for PSRAM chip select.
2. Add PSRAM zone to page allocator (separate from SRAM pages).
3. Implement zone preference: SRAM for hot pages, PSRAM for cold pages.
4. Test with many concurrent processes to exercise PSRAM pages.

**Verification**: `runtests` passes with >100 pages allocated from PSRAM.

### Phase Pi2-7: TrustZone (Extended)

**Goal**: Kernel runs in Secure world, user processes in Non-Secure.

1. Configure SAU: kernel memory → Secure, user memory → Non-Secure.
2. Syscall entry via `SG` (Secure Gateway) instruction.
3. Non-Secure process faults on kernel memory access.
4. Verify all tests still pass with TrustZone partitioning.

**Verification**: User process attempting kernel memory read triggers
SecureFault, not silent access.

---

## 8. Code Size Comparison

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

## 9. Risks and Open Questions

### 9.1 Pico SDK Version

The Pico 2 requires Pico SDK 2.0+, which supports both RP2040 and RP2350.
PPAP currently uses Pico SDK 1.x.  Upgrading the SDK may introduce build
changes.

**Mitigation**: The SDK upgrade is a one-time step.  Pico SDK 2.0 is
backward-compatible with RP2040 projects.

### 9.2 Flash Layout

The RP2350 boot ROM may use a different flash layout (boot2 size,
alignment).  The stage1 VTOR address may need adjustment.

**Mitigation**: Check the RP2350 datasheet for boot ROM behavior.  The
Pico SDK handles boot2 generation automatically.

### 9.3 PSRAM Latency

PSRAM access is significantly slower than SRAM (~5–10× for random access).
If the page allocator naively assigns PSRAM pages to active processes,
performance degrades.

**Mitigation**: Zone-based allocation with SRAM preference for hot pages.
Only spill to PSRAM when SRAM is exhausted.

### 9.4 TrustZone Complexity

TrustZone adds complexity to the syscall path and exception handling.
The Secure/Non-Secure transition has overhead (~20 cycles per `SG`).

**Mitigation**: TrustZone is an extended goal.  The initial port runs
everything in Secure world, identical to the RP2040 port.

### 9.5 RISC-V Support

The Hazard3 RISC-V cores are a completely different architecture.  Adding
RISC-V support requires a new `src/arch/rv32/` directory with boot, trap,
and context switch code — comparable effort to the original m68k port.

**Mitigation**: RISC-V is explicitly out of scope for this proposal.
A separate `docs/proposals/riscv-port.md` should be created when ready.

---

## 10. Dependency Graph

```
Pi2-1 (boot + UART)
  └─→ Pi2-2 (ARMv8-M MPU)
        └─→ Pi2-3 (dual-core + tests)
              ├─→ Pi2-4 (FPU)
              ├─→ Pi2-5 (PicoCalc 2)
              └─→ Pi2-6 (PSRAM)
                    └─→ Pi2-7 (TrustZone)
```

Pi2-1 through Pi2-3 are the core port.  Everything else is independent
extensions.  The core port should be quick (~1–2 steps) because the
RP2350 is so similar to the RP2040.

---

## 11. Related Documentation

- [docs/kernel/overview.md](../kernel/overview.md) — PPAP kernel architecture
- [docs/reference/picocalc.md](../reference/picocalc.md) — PicoCalc hardware reference
- [docs/proposals/pizero_port.md](pizero_port.md) — Pi Zero port (ARM1176, MMU — different scope)
