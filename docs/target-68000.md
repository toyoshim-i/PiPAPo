# Motorola 68000 Target

Architecture-specific reference for the PPAP m68k port. The current target
is QEMU (`virt` machine, `-cpu m68000`), with X68000 (Sharp X68000) as the
eventual hardware target.

For the original pre-implementation planning document, see
[history/target-68000-plan.md](history/target-68000-plan.md).

---

## 1. Architecture Comparison

| Aspect | ARM Cortex-M0+ | Motorola 68000 |
|--------|----------------|----------------|
| ISA | 16-bit Thumb-1 (RISC) | 32-bit CISC |
| Word size | 32-bit | 32-bit (16-bit data bus on 68000) |
| Endianness | Little-endian | **Big-endian** |
| Registers | r0-r12, SP, LR, PC, xPSR | d0-d7 (data), a0-a6, a7/SP, PC, SR |
| Callee-saved | r4-r11 | d2-d7, a2-a6 |
| Caller-saved | r0-r3, r12, LR | d0-d1, a0-a1 |
| Stack pointer | MSP/PSP (dual) | USP/SSP (dual: user/supervisor) |
| Privilege | Handler/Thread mode | Supervisor/User mode (SR bit 13) |
| Syscall | `svc 0` | `trap #0` |
| Timer interrupt | SysTick (built-in) | External timer IC |
| Context switch | PendSV (deferred) | Direct switch in timer ISR |
| MMU/MPU | 4-region MPU | None (68000); 68030+ has full MMU |
| Address space | 32-bit, XIP from flash | 24-bit (16 MB) on 68000 |
| Vector table | Configurable via VTOR | Fixed at 0x000000 |
| HW exception frame | Auto-push {r0-r3,r12,lr,pc,xpsr} | Auto-push {SR,PC} only |
| PIC convention | r9 = GOT base | a5 = data base (`-msep-data`) |
| ELF machine | EM_ARM (40) | EM_68K (4) |
| Relocation | R_ARM_RELATIVE (23) | R_68K_RELATIVE (22) |
| Compiler flags | `-mthumb -mcpu=cortex-m0plus` | `-m68000` |

---

## 2. Syscall ABI

PPAP uses `trap #0` for system calls on m68k, matching the Linux m68k / musl
convention. This means musl's syscall wrappers work unmodified.

```
trap #0
d0 = syscall number → return value
d1 = arg1
d2 = arg2
d3 = arg3
d4 = arg4
d5 = arg5
a0 = arg6
```

### TRAP Number Rationale

TRAP #0 was chosen because it is free on all potential 68k target platforms:

| TRAP # | Atari ST (TOS) | X68000 (Human68k) | Classic Mac | Amiga |
|--------|---------------|-------------------|-------------|-------|
| 0 | free | user-defined | free | free |
| 1 | **GEMDOS** | user (mpcm.x TSR) | free | free |
| 13 | **BIOS** | **CTRL+C** | free | free |
| 14 | **XBIOS** | **error handler** | free | free |
| 15 | free | **IOCS** | free | free |

Notes:
- **Classic Mac** uses A-line exceptions for OS traps, not TRAP instructions
- **Amiga** uses library base jumptables (ExecBase at address 4)
- **X68000** Human68k DOS calls use F-line exceptions ($FFxx), not TRAP
- X68000 TRAP #1-4 are commonly hijacked by popular TSR sound drivers

### Argument Count

m68k supports 6 syscall arguments (d1-d5, a0), while ARM supports 6
(r0-r5). For syscalls requiring 6 arguments (e.g., mmap2), the 6th
argument is passed in a0.

---

## 3. Context Switch

### PCB Register Layout

```
PCB offset  0..39:  d2, d3, d4, d5, d6, d7, a2, a3, a4, a5 (10 regs × 4B = 40B)
PCB offset 40:      a6 (frame pointer, callee-saved)
PCB offset 44:      sp (saved SSP)
PCB_SP_OFFSET = 44
```

### Preemptive Switch

The QEMU virt machine provides a Goldfish PIT timer. The timer ISR performs
a direct context switch (no PendSV equivalent on 68k):

1. Save all registers (d0-d7/a0-a6) + USP to supervisor stack
2. Call `sched_tick()` → decides whether to switch
3. If switching: save SP to current PCB, call `sched_next()`, load SP from next PCB
4. Restore all registers + USP, `rte`

Cooperative yield uses the same save/restore path, triggered from C code.

---

## 4. Endianness Strategy

The 68000 is big-endian. PPAP uses per-format endianness:

| Format | Endianness | Rationale |
|---|---|---|
| ELF | Target-native | Big for m68k, little for ARM; `elf_validate()` checks `EI_DATA` |
| romfs | **Target-native** | Built per-target by `mkromfs`; not shared across architectures. Native reads = zero overhead |
| UFS | **Always little-endian** | UFS images on FAT32 SD cards may be shared between ARM and m68k. Fixed LE matches ext2 convention |
| FAT32/VFAT | **Always little-endian** | Per FAT32 spec (mandatory) |

Endian-aware accessors (`le16()`/`le32()` in `src/kernel/endian.h`) are used
by UFS and VFAT drivers. romfs and ELF use native reads (no conversion).

Host tools:
- `mkromfs` — writes target-native endian (`--big-endian` flag for m68k builds)
- `mkufs` — always writes little-endian

---

## 5. QEMU Target (`qemu_m68k`)

### Machine and CPU

```
qemu-system-m68k -machine virt -cpu m68000 -nographic -kernel ppap_qemu_m68k.elf
```

The `virt` machine provides a minimal platform with a Goldfish UART and
Goldfish PIT timer. The `-cpu m68000` flag ensures pure 68000 ISA (no
ColdFire or 68020+ instructions).

### Memory Map

| Region | Address | Size | Purpose |
|---|---|---|---|
| RAM | 0x00000000 | Up to 16 MB (auto-detected) | Vector table + kernel + romfs + page pool |
| UART | 0xFF008000 | 4 KB | Goldfish TTY (console) |

### RAM Detection

RAM size is auto-detected at boot by `m68k_probe_ram()` (`src/arch/m68k/probe_ram.S`):

1. **Coarse phase** — 1 MB steps to find the approximate boundary
2. **Fine phase** — 4 KB steps to find the exact page-aligned boundary

Each step writes a pattern (0xA5F01234), reads it back, and restores the
original value. This works on both emulators (QEMU returns 0 for unmapped
reads) and real hardware (bus errors on unmapped access).

The probe ceiling is bounded by `RAM_END` (target-configurable via CMake
`-DRAM_END=...`). For X68000, `RAM_END=0xC00000` excludes VRAM.

`PAGE_COUNT_MAX` (compile-time, 4096 for QEMU = 16 MB capacity) sizes the
static free-stack array; `page_count` (runtime) holds the detected count.

### Target Files

```
src/arch/m68k/
  boot.S              — vector table + reset handler (.data copy, .bss zero)
  switch.S            — context switch (save/restore d2-d7/a2-a6/USP)
  trap.S              — TRAP #0 syscall handler
  probe_ram.S         — RAM size detection
  cpu.h               — SR bits, exception frame layout
  arch.h              — IRQ save/restore, yield, core_id, WFI
  math.S              — 32-bit multiply/divide (replaces libgcc)

src/target/qemu_m68k/
  target_qemu_m68k.c  — target hooks (early_init, late_init, etc.)
  qemu_m68k.ld        — linker script (RAM-only: 0x00000000)
  CMakeLists.txt       — target build rules
  drivers/
    uart_qemu_m68k.c  — Goldfish UART driver
```

### Toolchain

```
m68k-elf-gcc          — bare-metal cross compiler (custom-built)
m68k-elf-as / m68k-elf-ld / m68k-elf-gdb
qemu-system-m68k      — apt install qemu-system-m68k
```

The m68k toolchain is a custom-built `m68k-elf-gcc` targeting bare-metal
(no libc). Build with `scripts/build-m68k-toolchain.sh`, which produces a
68000-safe toolchain (no 68020+ instructions in libgcc). The `gcc-m68k-linux-gnu`
package from apt targets Linux userspace and is **not** suitable for
bare-metal kernel builds.

---

## 6. User-Space Details

### Compiler Flags

```sh
CFLAGS="-m68000 -fPIC -msep-data"
```

The `-msep-data` flag separates text and data segments with a5 as the data
base register (analogous to ARM's `-msingle-pic-base -mpic-register=r9`).

### ELF Format

| Field | Value |
|-------|-------|
| Class | ELF32 |
| Data | Big-endian (ELFDATA2MSB) |
| Machine | EM_68K (4) |
| Type | ET_DYN (PIE) |
| Relocation | R_68K_RELATIVE |

Both `.text` and `.data` are loaded into RAM (no XIP). Code can execute
directly from the romfs region in RAM without per-process copying; only
writable `.data`/`.bss` needs per-process pages.

---

## 7. X68000 Target (`x68k`) — Future

### Hardware Overview

| Item | Specification |
|---|---|
| CPU | Motorola 68000 @ 10 MHz (original), 68030 @ 25 MHz (X68030) |
| RAM | 1-12 MB (main), 1 MB TVRAM, 512 KB GVRAM |
| ROM | 128 KB IPL ROM (0xFE0000-0xFFFFFF) |
| Storage | 5.25" floppy (1.2 MB), SASI/SCSI HDD |
| Display | Custom CRTC, 768×512 max, 65536 colors |
| Sound | YM2151 (FM) + ADPCM |
| Keyboard | Serial via 8255 PPI |
| Serial | 8251 USART |
| Timer | 8253 PIT (3 channels) + MFP (68901) timers |

### Memory Map

| Address | Size | Contents |
|---|---|---|
| 0x000000 | 1-12 MB | Main RAM |
| 0xC00000 | 1 MB | GVRAM (graphics) |
| 0xE00000 | 512 KB | TVRAM (text) |
| 0xE80000 | 128 KB | I/O area (peripherals) |
| 0xEB0000 | 64 KB | Sprite/BG/PCG RAM |
| 0xED0000 | 16 KB | SRAM (battery-backed) |
| 0xFE0000 | 128 KB | IPL ROM |

Key I/O addresses: MFP (0xE88000), PPI (0xE9A000), DMAC (0xE84000),
CRTC (0xE80000), YM2151 (0xE90000), FDC (0xE94000), SASI (0xE96000).

### Boot Sequence

The X68000 IPL ROM reads the first 1 KB from floppy and loads it to
address $002000, then jumps there. The bootstrap uses IOCS calls (TRAP #15)
to read the kernel from subsequent sectors and jump to it.

**Floppy layout:**

| Region | Offset | Size | Contents |
|---|---|---|---|
| Bootstrap | 0 KB | 1 KB | IPL loads to $002000 |
| Kernel | 1 KB | ~64 KB | Loaded by bootstrap |
| romfs | ~65 KB | ~1167 KB | Root filesystem |

Floppy format: 77 tracks × 2 heads × 8 sectors × 1024 bytes = 1,232 KB.

### IOCS Strategy

The X68000 IPL ROM provides IOCS (Input/Output Control System) via TRAP #15.
PPAP should use IOCS as much as possible rather than writing bare-metal drivers:

| Function | IOCS call | Benefit |
|---|---|---|
| Floppy read/write | `_B_READ` / `_B_WRITE` | No FDC driver needed |
| Serial I/O | `_B_PUTC` / `_B_GETC` | Early console without MFP driver |
| Keyboard input | `_B_KEYSNS` / `_B_KEYINP` | No PPI driver needed |
| Display output | `_B_PUTMES` / `_B_LOCATE` | Text output without CRTC driver |
| Timer | `_TIMERDST` | Timer-C interrupt setup |

IOCS calls are non-reentrant and run in supervisor mode — the kernel must
hold a lock or disable interrupts around IOCS calls from process context.
Custom drivers (direct TVRAM writes, DMA floppy) can replace IOCS later
for performance.

### Human68k Binary Compatibility

As a stretch goal, PPAP on X68000 can run existing Human68k executables
(.x format) by intercepting their F-line exception ($FFxx) DOS calls.
See [feature-subsystem.md](feature-subsystem.md) for the full subsystem
design including the Human68k personality layer.

### Emulators for Development

- **XM6 TypeG** — accurate X68000 emulator (Windows, runs under Wine)
- **XEiJ** — Java-based emulator with debugging
- Custom QEMU with X68000 machine support (community patches)

---

## 8. Key Differences from ARM

| Aspect | ARM (RP2040) | m68k (QEMU / X68000) |
|---|---|---|
| Code execution | XIP from flash | RAM (from romfs or loaded) |
| Multi-core | Dual-core, HW spinlocks | Single-core |
| Memory protection | MPU (4 regions) | None |
| Timer | SysTick (built-in) | External (Goldfish PIT / 8253 PIT / MFP) |
| Context switch | PendSV (deferred lowest priority) | Direct in timer ISR |
| User/kernel transition | Thread mode + PSP / Handler mode + MSP | User mode (USP) / Supervisor mode (SSP) |
| Division | Software (libgcc) | Hardware (`divs`/`divu`) |

---

## Related Documentation

- [design.md](design.md) — Kernel internals (boot, memory, scheduler, signals)
- [userland-dev-guide.md](userland-dev-guide.md) — Compiler flags, PIC model, ELF details
- [syscall.md](syscall.md) — Complete syscall reference (shared numbering)
- [feature-eCPU.md](feature-eCPU.md) — CPU emulation layer (cross-arch execution)
- [feature-subsystem.md](feature-subsystem.md) — OS personality layers (Human68k, CP/M, DOS)
- [history/target-68000-plan.md](history/target-68000-plan.md) — Original planning document
