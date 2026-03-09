# Motorola 68000 Port — Target Plan

Porting PPAP to the Motorola 68000 architecture. Initial target is QEMU
(`mcf5208evb` or `virt-m68k`), with X68000 (Sharp X68000) as the eventual
hardware target.

---

## 1. Architecture Comparison

| Aspect | ARM Cortex-M0+ (current) | Motorola 68000 |
|--------|--------------------------|----------------|
| ISA | 16-bit Thumb-1 (RISC) | 32-bit CISC |
| Word size | 32-bit | 32-bit (16-bit data bus on 68000) |
| Endianness | Little-endian | **Big-endian** |
| Registers | r0-r12, SP, LR, PC, xPSR | d0-d7 (data), a0-a6, a7/SP, PC, SR |
| Callee-saved | r4-r11 | d2-d7, a2-a6 (convention) |
| Caller-saved | r0-r3, r12, LR | d0-d1, a0-a1 |
| Stack pointer | MSP/PSP (dual) | USP/SSP (dual: user/supervisor) |
| Privilege | Handler/Thread mode | Supervisor/User mode (SR bit 13) |
| Syscall | `SVC` instruction | `TRAP #N` instruction (N=0..15) |
| Timer interrupt | SysTick (built-in) | External timer IC (no built-in) |
| Context switch | PendSV (deferred exception) | Software interrupt or manual |
| MMU/MPU | 4-region MPU | None (68000); 68030+ has full MMU |
| Address space | 32-bit, XIP from flash | 24-bit (16 MB) on 68000 |
| Vector table | Configurable via VTOR | Fixed at 0x000000 (68000), VBR on 68010+ |
| HW exception frame | Auto-push {r0-r3,r12,lr,pc,xpsr} | Auto-push {SR,PC} only (4 bytes on 68000) |
| PIC convention | r9 = GOT base | a5 = GOT base (m68k ABI) |
| ELF machine | EM_ARM (40) | EM_68K (4) |
| Relocation | R_ARM_RELATIVE (23) | R_68K_RELATIVE (22) |

---

## 2. ARM-Specific Code Inventory

Every file that must be rewritten or branched for m68k:

### 2.1 Assembly — Full Rewrite

| Current file | Purpose | M68K equivalent |
|---|---|---|
| `src/boot/startup.S` | Vector table (256 entries on 68k), Reset handler, .data copy, .bss zero | Full rewrite: 68k vector format, `move.l` loops |
| `src/boot/stage1.S` | RP2040 VTOR redirect + boot2 | Not needed (68k boots from vector at 0x0) |
| `src/kernel/proc/switch.S` | PendSV context switch: save/restore r4-r11 via PSP | Full rewrite: save d2-d7/a2-a6, swap USP/SSP |
| `src/kernel/syscall/svc.S` | SVC exception entry: capture r4/r5/r7, call dispatch | Full rewrite: TRAP #0 handler, read syscall# from d0 |

### 2.2 Architecture Headers — Full Rewrite

| Current file | Purpose | M68K equivalent |
|---|---|---|
| `src/hw/cortex_m0plus.h` | SysTick, SCB, NVIC, SHPR registers | `m68k.h`: SR bits, VBR (68010+), exception frame layout |
| `src/hw/rp2040.h` | RP2040 peripheral register map | Not needed for m68k (replaced by target-specific headers) |

### 2.3 Kernel C — Partial Rewrite / Abstraction Needed

| Current file | ARM-specific code | M68K changes |
|---|---|---|
| `src/kernel/mm/mpu.c` | ARM MPU registers (0xE000ED90) | Stub (68000 has no protection); future: 68030 MMU |
| `src/kernel/proc/sched.c` | SysTick config, PendSV trigger (`SCB_ICSR \|= PENDSVSET`), EXC_RETURN for user/kernel tick accounting, inline asm (`cpsid i`, `cpsie i`, `msr psp`, `msr control`) | Timer init via target hook; trigger context switch via `TRAP #1` or software flag; SR bit 13 for user/kernel distinction; `move.w sr,d0` / `or.w #0x0700,sr` for IRQ disable |
| `src/kernel/proc/proc.h` | PCB layout: r4-r11 fields, PCB_SP_OFFSET=32, `proc_setup_stack` builds ARM exception frame | PCB layout: d2-d7/a2-a6 fields (11 regs × 4 = 44 bytes), PCB_SP_OFFSET=44, build 68k-style return frame |
| `src/kernel/proc/proc.c` | `proc_setup_stack`: builds {r4-r11} SW frame + {r0-r3,r12,lr,pc,xpsr} HW frame | Build 68k frame: {d2-d7,a2-a6} + {SR,PC} |
| `src/kernel/exec/elf.c` | Validates EM_ARM, ELFDATA2LSB, EF_ARM_EABI_VER5 | Validate EM_68K, ELFDATA2**MSB**; no EABI flags |
| `src/kernel/exec/elf.h` | `EM_ARM`, `R_ARM_RELATIVE`, `ELFDATA2LSB` | Add `EM_68K` (4), `R_68K_RELATIVE` (22), `ELFDATA2MSB` (2) |
| `src/kernel/exec/exec.c` | Thumb bit handling (`entry \|= e_entry & 1`), r9=GOT patching at sw[5] | No Thumb bit; a5=GOT patching at different offset |
| `src/kernel/spinlock.h` | RP2040 SIO hardware spinlocks, `cpsid i`/`cpsie i`, SCB.CPUID | 68k: `tas.b` instruction for spinlock; `or.w #0x0700,sr` for IRQ disable |
| `src/kernel/smp.c` | RP2040 SIO FIFO, PSM, Core 1 launch | Not applicable (68000 is single-core) |
| `src/kernel/klog.c` | May use inline asm for critical section | Use 68k IRQ disable |

### 2.4 ELF / Binary Format — Endianness

| Current file | Issue | M68K changes |
|---|---|---|
| `src/kernel/exec/elf.h` | All ELF structs assume native endian reads | 68k ELF is big-endian; host tools (mkromfs, mkufs) need byte-swap |
| `src/kernel/fs/romfs.c` | romfs header fields: assumed little-endian | Add endian-aware accessors or make romfs format target-endian |
| `src/kernel/fs/romfs_format.h` | On-disk format | Same — needs endian policy decision |
| `src/kernel/fs/ufs.c` / `ufs_format.h` | On-disk superblock, inodes | Same |
| `src/kernel/fs/vfat.c` / `vfat_format.h` | FAT32 is always little-endian per spec | Must byte-swap on big-endian 68k |

### 2.5 Drivers — Per-Target, Full Rewrite

| Current file | M68K status |
|---|---|
| `src/drivers/uart.c` | Replace with X68000 MFP/SCC UART or QEMU goldfish/pl011 |
| `src/drivers/clock.c` | RP2040 PLL — not applicable |
| `src/drivers/spi.c` | RP2040 SPI — not applicable |
| `src/drivers/sd.c` | X68000 uses SASI/SCSI, not SPI-SD |
| `src/drivers/i2c.c` | RP2040 I2C — not applicable |
| `src/drivers/lcd.c`, `spi_lcd.c`, `kbd.c`, `fbcon.c` | PicoCalc-specific — not applicable |

### 2.6 Target Layer — New Target Directories

| New directory | Contents |
|---|---|
| `src/target/qemu_m68k/` | `target_qemu_m68k.c`, `qemu_m68k.ld`, QEMU UART driver |
| `src/target/x68k/` | `target_x68k.c`, `x68k.ld`, MFP UART, keyboard, display drivers (future) |

### 2.7 Linker Scripts — Full Rewrite

68k memory map is fundamentally different:
- No XIP from flash (code runs from RAM or ROM)
- No split MSP/PSP stack regions
- X68000: ROM at 0xFE0000-0xFFFFFF, RAM at 0x000000-0x0BFFFF (768 KB typ.)

### 2.8 Build System

| File | Changes |
|---|---|
| `CMakeLists.txt` | Add `m68k-elf-gcc` toolchain detection, new targets |
| `cmake/kernel_sources.cmake` | Split `KERNEL_COMMON_SOURCES` to exclude arch-specific files; add `KERNEL_ARCH_ARM_SOURCES` and `KERNEL_ARCH_M68K_SOURCES` |
| New: `cmake/toolchain-m68k.cmake` | Cross-compilation toolchain file |

---

## 3. Code That Stays Unchanged (~80%)

These files have **no** architecture-specific code:

| Subsystem | Files |
|---|---|
| VFS layer | `vfs.c`, `vfs.h`, `namei.c` |
| Filesystems | `romfs.c`*, `devfs.c`, `procfs.c`, `tmpfs.c`, `ufs.c`*, `vfat.c`*, `fstab.c` |
| File descriptors | `fd.c`, `fd.h`, `file.h`, `tty.c`, `tty.h`, `pipe.c` |
| Block devices | `blkdev.c`, `blkdev.h`, `loopback.c`, `ramblk.c` |
| Syscall dispatch | `syscall.c`, `syscall.h`, all `sys_*.c` files |
| Signal handling | `signal.c` (C parts — delivery mechanism needs arch hooks) |
| Memory management | `page.c`, `page.h`, `kmem.c` (pure C page pool) |
| Kernel utilities | `klog.c`, `errno.h`, `config.h`, `main.c` |
| Target abstraction | `target.h` (the API itself is arch-neutral) |

\* Endianness accessors needed — see Section 5.

---

## 4. Proposed Architecture Abstraction

### 4.1 Directory Layout

```
src/
  arch/
    arm_m/
      boot.S            ← startup.S (renamed)
      switch.S           ← from proc/switch.S
      svc.S              ← from syscall/svc.S
      stage1.S           ← RP2040-only boot stage
      cpu.h              ← cortex_m0plus.h (renamed)
      spinlock.h         ← ARM-specific spinlock
      arch.h             ← arch interface (inline: irq_disable/enable, etc.)
    m68k/
      boot.S             ← 68k vector table + reset handler
      switch.S           ← 68k context switch
      trap.S             ← TRAP #0 handler (syscall entry)
      cpu.h              ← 68k SR bits, exception frame layout
      spinlock.h         ← tas.b based spinlock (single-core sufficient)
      arch.h             ← arch interface
  hw/
    rp2040.h             ← stays (RP2040 peripheral regs, used by drivers)
```

Note: the ARM Cortex-M (M-profile) directory is `arm_m/`, not `arm/`,
to distinguish it from the ARM1176 (A-profile) directory `arm_a/` used
by the Pi Zero port (see `target-pizero.md`).

### 4.2 Architecture Interface (`arch/*/arch.h`)

Common API that both ARM and m68k implement:

```c
/* IRQ control */
static inline uint32_t arch_irq_save(void);       /* disable IRQs, return old state */
static inline void arch_irq_restore(uint32_t);     /* restore saved IRQ state */
static inline void arch_irq_enable(void);
static inline void arch_irq_disable(void);

/* Context switch trigger */
static inline void arch_yield(void);               /* pend context switch */

/* Memory barriers */
static inline void arch_dsb(void);
static inline void arch_isb(void);

/* Core identification (multi-core: ARM only) */
static inline uint32_t arch_core_id(void);         /* 0 on 68k always */

/* Idle (wait for interrupt) */
static inline void arch_wfi(void);                 /* wfi on ARM, stop on 68k */
```

### 4.3 PCB Register Layout

**ARM (current)**:
```
PCB offset 0..31: r4, r5, r6, r7, r8, r9, r10, r11 (8 regs × 4B = 32B)
PCB offset 32:    sp (saved PSP)
PCB_SP_OFFSET = 32
```

**M68K**:
```
PCB offset 0..39:  d2, d3, d4, d5, d6, d7, a2, a3, a4, a5 (10 regs × 4B = 40B)
PCB offset 40:     a6 (frame pointer, also callee-saved)
PCB offset 44:     sp (saved USP or SSP)
PCB_SP_OFFSET = 44
```

This means `proc.h` needs the register save area to be arch-dependent (either
via `#ifdef` or a typedef in `arch.h`).

### 4.4 Exception / Syscall Entry

**ARM (current)**:
- Hardware auto-saves {r0-r3, r12, LR, PC, xPSR} to PSP (32 bytes)
- SVC handler reads r7 for syscall number (still live, not in HW frame)
- Return via `bx lr` with EXC_RETURN magic value

**M68K**:
- Hardware auto-saves {SR, PC} to SSP (6 bytes on 68000; 4-word on 68010+)
- TRAP #0 handler must manually save all registers it needs
- Syscall number in d0, arguments in d1-d4 (or a0-a1 for pointers)
- Return value in d0
- Return via `rte` instruction

### TRAP Number Selection

The 68000 provides TRAP #0 through TRAP #15. Choosing the right number
requires avoiding conflicts with all potential target platforms:

| TRAP # | Atari ST (TOS) | X68000 (Human68k) | Classic Mac | Amiga |
|--------|---------------|-------------------|-------------|-------|
| 0 | free | user-defined | free | free |
| 1 | **GEMDOS** | user (mpcm.x TSR) | free | free |
| 2 | **AES/VDI** | user (pcm8.x TSR) | free | free |
| 3 | free | user (zmusic TSR) | free | free |
| 4 | free | user (mxdrv TSR) | free | free |
| 5 | free | user (free) | free | free |
| 6 | free | user (free) | free | free |
| 7 | free | user (free) | free | free |
| 8 | free | **breakpoint** | free | free |
| 9 | free | **breakpoint** | free | free |
| 10 | free | **reset/power-off** | free | free |
| 11 | free | **BREAK key** | free | free |
| 12 | free | **COPY key** | free | free |
| 13 | **BIOS** | **CTRL+C** | free | free |
| 14 | **XBIOS** | **error handler** | free | free |
| 15 | free | **IOCS** | free | free |

Notes on platforms that don't use TRAP instructions for system calls:
- **Classic Mac** uses A-line exceptions ($Axxx illegal opcodes) for Toolbox/OS traps
- **Amiga** uses library base jumptables (ExecBase at address 4)
- **X68000** Human68k DOS calls use F-line exceptions ($FFxx), not TRAP
- **X68000** TRAP #0-7 are "user-defined" but #1-4 are commonly hijacked
  by popular TSR sound/music drivers (mpcm, pcm8, zmusic, mxdrv)

**Decision: TRAP #0.** Rationale:
1. Free on all four target platforms (no OS or common TSR conflicts)
2. Matches the **Linux m68k** convention — musl libc already uses `trap #0`
   with d0=syscall_nr, so the syscall wrapper needs zero modification
3. Not used by any popular X68000 TSR (those use TRAP #1-4)

Syscall ABI (matches Linux m68k / musl):
```
trap #0
d0 = syscall number
d1 = arg1
d2 = arg2
d3 = arg3
d4 = arg4
d5 = arg5
d0 = return value (negative = -errno)
```

Note: m68k supports 5 syscall arguments (d1-d5), while ARM supports 6
(r0-r5). PPAP syscalls requiring 6 arguments (e.g., mmap) must use a
pointer-to-struct argument on m68k, matching the Linux m68k convention.

### 4.5 Context Switch

**ARM (current)**: PendSV fires at lowest priority, saves r4-r11 to PSP,
calls `sched_next()`, restores next process, `bx lr` with EXC_RETURN.

**M68K**: No PendSV equivalent. Options:
1. **Software interrupt**: set a flag in the timer ISR, check on `rte` return
   path, and branch to the context switch routine.
2. **Direct switch in timer ISR**: save full context (d2-d7, a2-a6, USP) in
   the timer handler, call `sched_next()`, restore next context, `rte`.
3. **TRAP #1**: use a separate trap for yield/context switch.

Recommended: **Option 2** (direct switch in timer ISR), with **TRAP #1** for
cooperative `sched_yield()`. This is simplest and matches classic 68k OS
designs (AmigaOS, TOS).

```asm
@ Timer ISR (68k pseudocode)
timer_isr:
    movem.l  d0-d7/a0-a6,-(sp)    | save all registers to SSP
    move.l   usp,a0
    move.l   a0,-(sp)              | save USP

    jsr      sched_tick            | may decide to switch
    tst.l    d0                    | switch needed?
    beq.s    .no_switch

    | save SP to current PCB
    move.l   current,a0
    move.l   sp,PCB_SP_OFFSET(a0)

    jsr      sched_next            | d0 = next pcb_t*
    move.l   d0,current

    | restore SP from next PCB
    move.l   d0,a0
    move.l   PCB_SP_OFFSET(a0),sp

.no_switch:
    move.l   (sp)+,a0
    move.l   a0,usp                | restore USP
    movem.l  (sp)+,d0-d7/a0-a6    | restore all registers
    rte
```

---

## 5. Endianness Strategy

The 68000 is big-endian. This affects:

### 5.1 Prior Art: Filesystem Endianness in Unix

| Filesystem | On-disk endianness | Notes |
|---|---|---|
| BSD FFS/UFS | **Native** (not portable) | SPARC UFS can't mount on x86 |
| Linux ext2/3/4 | **Always little-endian** | Cross-arch portability |
| XFS | **Always big-endian** | SGI MIPS heritage |
| FAT32/VFAT | **Always little-endian** | IBM PC spec |

Traditional BSD UFS uses native endian — no cross-architecture portability.
Linux chose always-little-endian for ext2 to solve this. PPAP's UFS is
inspired by 4.4BSD FFS but is its own format, so we choose freely.

### 5.2 On-Disk Formats

| Format | Endianness | Rationale |
|---|---|---|
| ELF | Target-native | big for m68k, little for ARM; `elf_validate()` checks `EI_DATA` |
| romfs | **Target-native** | Built per-target by `mkromfs`; not shared across architectures. Native reads = zero overhead, simpler code |
| UFS | **Always little-endian** | UFS images live on FAT32 SD cards that may be shared between ARM and m68k targets. Fixed endianness enables portability. Little-endian matches ext2 convention and existing ARM images |
| FAT32/VFAT | **Always little-endian** | Per spec (mandatory) |

### 5.3 Rationale

**romfs = native endian**: romfs is generated at build time by `mkromfs` for
a specific target and baked into the flash/floppy image. There is no use case
for mounting an ARM romfs on m68k. Native reads mean no byte-swap overhead
and simpler code in the hot path (romfs is the root filesystem).

**UFS = always little-endian**: UFS images live on FAT32 SD cards. A user
might create a UFS image on an ARM target and later mount it on m68k (or
vice versa via a shared SD card). Fixed little-endian ensures portability.
The byte-swap cost is negligible for UFS I/O (already bottlenecked by SD).

### 5.4 Implementation

Add a shared header for endian-aware accessors (used by UFS and VFAT):

```c
/* src/kernel/endian.h */
#ifdef __BIG_ENDIAN__
static inline uint16_t le16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t le32(uint32_t v) { return __builtin_bswap32(v); }
#else
static inline uint16_t le16(uint16_t v) { return v; }
static inline uint32_t le32(uint32_t v) { return v; }
#endif
```

Files that need `le16()`/`le32()` accessors:
- `vfat.c` — already little-endian per FAT32 spec (add explicit accessors)
- `ufs.c` — wrap all on-disk field reads/writes
- `ufs_format.h` — document that on-disk fields are little-endian

Files that read natively (no accessors needed):
- `romfs.c` — target-native endian, no conversion
- `elf.c` — target-native endian ELF binaries

Host tools:
- `mkromfs` — writes target-native endian (add `--big-endian` flag for m68k)
- `mkufs` — always writes little-endian (no change needed, already runs on LE host)

---

## 6. QEMU M68K Target (`qemu_m68k`)

### 6.1 QEMU Machine

QEMU provides several m68k machines:
- **`virt`** — minimal virtual platform, UART at MMIO, simple timer
- **`mcf5208evb`** — ColdFire 5208 evaluation board (ColdFire ≠ 68000 ISA,
  but close enough for initial bringup; ISA-A compatible with 68000 subset)

Recommended: **`virt`** if available in the QEMU version, otherwise
`mcf5208evb`. Check `qemu-system-m68k -machine help`.

### 6.2 Memory Map (QEMU virt)

| Region | Address | Size | Purpose |
|---|---|---|---|
| RAM | 0x00000000 | Up to 16 MB (runtime-detected) | Vector table + kernel + romfs + page pool |
| UART | 0xFF008000 | 4 KB | Goldfish TTY |

RAM size is auto-detected at boot by `m68k_probe_ram()` (assembly routine in
`src/arch/m68k/probe_ram.S`). The probe uses a two-phase approach for efficiency:

1. **Coarse phase** — 1 MB steps to find the approximate RAM boundary
2. **Fine phase** — 4 KB steps to find the exact page-aligned boundary

Each step writes a unique pattern (0xA5F01234), reads it back to verify, and
restores the original value. This handles both real hardware (bus errors on
unmapped access) and emulators (QEMU returns 0 for unmapped reads without
generating a bus error).

The probe range is bounded by `RAM_END` (target-configurable via CMake
`-DRAM_END=...`). Default: `PAGE_POOL_BASE + PAGE_COUNT_MAX * PAGE_SIZE`.
For X68000, `RAM_END=0xC00000` excludes VRAM.

`PAGE_COUNT_MAX` (compile-time, 4096 for QEMU = 16 MB capacity) sizes the
static free-stack array; `page_count` (runtime) holds the actual detected
page count.

### 6.3 Target Files

```
src/target/qemu_m68k/
  target_qemu_m68k.c    — target hooks (early_init, late_init, etc.)
  qemu_m68k.ld          — linker script (RAM-only: 0x00000000)
  drivers/
    uart_qemu_m68k.c    — QEMU UART driver
```

### 6.4 Toolchain

```
m68k-elf-gcc            — bare-metal cross compiler (custom-built)
m68k-elf-as / m68k-elf-ld
m68k-elf-gdb            — debugger
qemu-system-m68k        — emulator
```

The m68k toolchain is a custom-built `m68k-elf-gcc` (bare-metal, no libc).
Build it with `third_party/build-gcc-m68k.sh`, which produces a 68000-safe
toolchain (no 68020+ instructions in libgcc). The `gcc-m68k-linux-gnu`
package from apt targets Linux userspace and is **not** suitable for
bare-metal kernel builds.

QEMU: `apt install qemu-system-m68k`.

---

## 7. X68000 Target (`x68k`) — Future

### 7.1 Hardware Overview

| Item | Specification |
|---|---|
| CPU | Motorola 68000 @ 10 MHz (original), 68030 @ 25 MHz (X68030) |
| RAM | 1-12 MB (main), 1 MB TVRAM, 512 KB GVRAM |
| ROM | 1 MB IPL ROM (0xFE0000-0xFFFFFF) |
| Storage | 5.25" floppy (1.2 MB), SASI/SCSI HDD |
| Display | Custom CRTC, 768×512 max, 65536 colors |
| Sound | YM2151 (FM) + ADPCM |
| Keyboard | Serial (directly connected to 8255 PPI) |
| Serial | 8251 USART |
| Timer | 8253 PIT (3 channels) |
| Interrupt controller | Custom (directly handled by CPU via autovectors) |

### 7.2 X68000 Memory Map

| Address | Size | Contents |
|---|---|---|
| 0x000000 | 1-12 MB | Main RAM |
| 0xC00000 | 1 MB | GVRAM (graphics) |
| 0xE00000 | 512 KB | TVRAM (text) |
| 0xE80000 | 128 KB | I/O area (peripherals) |
| 0xEB0000 | 64 KB | Sprite/BG/PCG RAM |
| 0xED0000 | variable | SRAM (battery-backed, 16 KB) |
| 0xF00000 | 768 KB | (reserved) |
| 0xFC0000 | 128 KB | User ROM area |
| 0xFE0000 | 128 KB | IPL ROM |

Key I/O addresses:
- MFP (68901): 0xE88000 — timer, UART, GPIO
- 8255 PPI: 0xE9A000 — joystick, keyboard
- 8253 PIT: 0xE9A000 area — programmable timer
- DMAC (68450): 0xE84000
- CRTC: 0xE80000
- Palette: 0xE82000
- ADPCM: 0xE92000
- YM2151: 0xE90000
- FDC: 0xE94000
- SASI: 0xE96000

### 7.3 Floppy Boot Sequence

The X68000 IPL ROM handles floppy boot as follows:

**Step 1 — IPL ROM reads boot sector:**
The IPL ROM reads the first 1 KB (sector 0 of track 0) from the floppy
and loads it to address **$002000** in main RAM, then jumps to $002000.

(For SASI/SCSI boot, the load address is $002400 instead.)

**Step 2 — Boot sector code (our bootstrap):**
The 1 KB bootstrap must be position-independent or linked at $002000.
It is responsible for loading the kernel from subsequent sectors on the
floppy into RAM at a chosen address (e.g., $010000) and jumping to it.

**Floppy disk format:**
```
77 tracks × 2 heads × 8 sectors/track × 1024 bytes/sector = 1,232 KB
```

Sectors are 1024 bytes (not 512 like PC floppies). The boot sector
occupies track 0, head 0, sector 1 (CHS numbering starts at 1 for
sectors, 0 for tracks and heads).

**Proposed PPAP floppy layout:**

| Sector(s) | Offset | Size | Contents |
|---|---|---|---|
| T0/H0/S1 | 0 KB | 1 KB | Bootstrap (IPL loads to $002000) |
| T0/H0/S2-S8 + more | 1 KB | ~64 KB | Kernel binary |
| Remaining | ~65 KB | ~1167 KB | romfs image |

The bootstrap code:
1. Uses IOCS calls (TRAP #15) to read kernel sectors from floppy
   (the IOCS is available after IPL ROM init)
2. Copies kernel to target address (e.g., $010000)
3. Sets up initial SSP from kernel vector table
4. Jumps to kernel Reset_Handler

This is analogous to the RP2040 boot2 + stage1: a small loader in a
fixed location that sets up the environment and hands off to the kernel.

**NetBSD/x68k reference:** NetBSD's `sys/arch/x68k/stand/boot_ufs/boot.S`
uses the same IPL mechanism — the first 1 KB at $002000 bootstraps
loading of a larger `/boot` secondary loader. The boot sector includes
a `"SHARP/X680x0"` signature string (not strictly required by IPL ROM,
but conventional).

### 7.4 IOCS Strategy

The X68000 IPL ROM provides IOCS (Input/Output Control System) — a
firmware API accessible via TRAP #15 with function number in d0.b.
IOCS provides low-level hardware access (floppy I/O, display, keyboard,
timer, serial) that remains resident in ROM at all times.

**Policy: use IOCS as much as possible.** Rather than writing bare-metal
drivers for every X68000 peripheral, PPAP should call IOCS for:

| Function | IOCS call | Benefit |
|---|---|---|
| Floppy read/write | `_B_READ` / `_B_WRITE` | No FDC driver needed |
| Serial I/O | `_B_PUTC` / `_B_GETC` | Early console without MFP driver |
| Keyboard input | `_B_KEYSNS` / `_B_KEYINP` | No PPI driver needed |
| Display output | `_B_PUTMES` / `_B_LOCATE` | Text output without CRTC driver |
| Timer | `_TIMERDST` | Timer-C interrupt setup |
| CRTC init | `_CRTMOD` | Display mode setup |

This dramatically reduces the amount of X68000-specific driver code.
PPAP only needs custom drivers where IOCS is insufficient (e.g.,
direct TVRAM writes for a fast framebuffer console, or DMA-based
floppy I/O for performance).

IOCS calls run in supervisor mode and are non-reentrant, so the kernel
must hold a lock (or disable interrupts) around IOCS calls from
process context.

### 7.5 Human68k Binary Compatibility

As a stretch goal, PPAP on X68000 can run existing Human68k executables
(.x format) by intercepting their DOS calls.

**How Human68k DOS calls work:**
Human68k programs invoke DOS functions using F-line instructions —
`dc.w $FFxx` where `$xx` is the function number (e.g., `$FF02` =
`_PUTCHAR`, `$FF3D` = `_OPEN`). The 68000 CPU triggers a "line 1111
emulator" exception (vector 11, address $002C) when it encounters
these illegal opcodes.

**PPAP compatibility shim:**

1. **F-line exception handler**: Install a handler at vector 11 that
   decodes the `$FFxx` opcode from the instruction stream (read the
   word at the exception PC).

2. **DOS call translation table**: Map Human68k DOS call numbers to
   PPAP syscalls:

   | Human68k DOS call | Number | PPAP syscall |
   |---|---|---|
   | `_EXIT` | $FF00 | `_exit()` |
   | `_GETCHAR` | $FF01 | `read(0, ...)` |
   | `_PUTCHAR` | $FF02 | `write(1, ...)` |
   | `_OPEN` | $FF3D | `open()` |
   | `_CLOSE` | $FF3E | `close()` |
   | `_READ` | $FF3F | `read()` |
   | `_WRITE` | $FF40 | `write()` |
   | `_SEEK` | $FF42 | `lseek()` |
   | `_MALLOC` | $FF48 | `brk()` / `mmap()` |
   | `_EXEC` | $FF4B | `execve()` |
   | ... | ... | ... |

3. **Argument translation**: Human68k passes arguments on the stack
   (pushed before the `dc.w $FFxx` instruction). The shim reads
   arguments from the user stack, translates them to PPAP syscall
   register convention, and dispatches.

4. **X-format loader**: Human68k executables use the `.x` (X-format)
   binary format, not ELF. A minimal X-format loader reads the header,
   loads text/data/bss segments, applies relocations, and jumps to the
   entry point. The X-format header is simpler than ELF (~64 bytes).

This is analogous to how Linux's `binfmt_misc` or FreeBSD's Linux
emulation layer works — intercept foreign syscall conventions and
translate to native ones. The F-line exception makes this particularly
clean on 68k: no binary patching needed, the CPU traps automatically.

**Limitations:**
- Programs using IOCS directly (TRAP #15) work natively (IOCS is
  always available in ROM)
- Programs using undocumented Human68k internals won't work
- Memory management differences (Human68k uses a different heap model)
- TSR programs that hook interrupt vectors need careful handling

### 7.6 Key Differences from QEMU Target

| Aspect | QEMU m68k | X68000 |
|---|---|---|
| Timer | QEMU-provided | 8253 PIT or MFP timer |
| UART | QEMU virtual | MFP (68901) USART |
| Storage | RAM block device | Floppy (FDC) or SCSI |
| Display | None (serial only) | CRTC + TVRAM text console |
| Keyboard | None | 8255 PPI serial scan |
| Boot | Direct load by QEMU | IPL ROM → floppy/SCSI boot |

### 7.7 Emulators for X68000 Development

- **XM6 TypeG** — accurate X68000 emulator (Windows, runs under Wine)
- **XEiJ** — Java-based emulator with debugging
- Custom QEMU with X68000 machine support (community patches exist)

---

## 8. Implementation Plan

### Phase A — Architecture Abstraction (no new code yet)

Refactor existing ARM code into `src/arch/arm_m/` without changing behavior
(M-profile, to distinguish from Pi Zero's A-profile `arm_a/`):

1. Move `startup.S` → `src/arch/arm_m/boot.S`
2. Move `switch.S` → `src/arch/arm_m/switch.S`
3. Move `svc.S` → `src/arch/arm_m/trap.S`
4. Move `cortex_m0plus.h` → `src/arch/arm_m/cpu.h`
5. Extract `arch.h` interface from `spinlock.h` and `sched.c` inline asm
6. Make `proc.h` PCB register area arch-dependent
7. Update CMake to use `src/arch/arm_m/` sources
8. Verify all three existing targets still build and pass tests

### Phase B — M68K QEMU Bringup

1. Write `src/arch/m68k/boot.S` — vector table + reset handler
2. Write `src/arch/m68k/cpu.h` — SR bits, exception frame
3. Write `src/arch/m68k/arch.h` — IRQ save/restore, yield, WFI
4. Write `src/arch/m68k/spinlock.h` — `tas.b` based (or IRQ-disable only)
5. Write `qemu_m68k.ld` — RAM-only layout
6. Write `target_qemu_m68k.c` — UART init, target hooks
7. Write QEMU UART driver
8. Boot to `kmain()` with UART output: "PiPAPo booting... (m68k)"

### Phase C — Kernel Services on M68K

1. Port page allocator (should work as-is, pure C)
2. Write `src/arch/m68k/switch.S` — context switch
3. Write `src/arch/m68k/trap.S` — TRAP #0 syscall handler
4. Port `proc_setup_stack()` for 68k frame layout
5. Add endian accessors (`src/kernel/endian.h`)
6. Port romfs with endian accessors — verify read-only mount
7. Timer ISR + preemptive scheduling
8. Syscall dispatch — verify with a simple write(1, "hello", 5)

### Phase D — User-Space on M68K

1. Port ELF loader for EM_68K + big-endian + R_68K_RELATIVE
2. Build a minimal "hello world" ELF for m68k
3. exec() → user mode → TRAP #0 syscall → write → UART output
4. Port musl libc for m68k (musl already has m68k support upstream)
5. Build busybox for m68k (static, musl)
6. Interactive shell on QEMU

### Phase E — X68000 Hardware (future)

1. Floppy bootstrap (1 KB at $002000, uses IOCS TRAP #15 to load kernel)
2. Early console via IOCS `_B_PUTC` / `_B_GETC` (no driver needed)
3. Timer via IOCS `_TIMERDST` (Timer-C interrupt for scheduler)
4. Keyboard via IOCS `_B_KEYSNS` / `_B_KEYINP`
5. Floppy block device via IOCS `_B_READ` / `_B_WRITE`
6. Boot from floppy image on real hardware / XM6 emulator
7. TVRAM direct-write console (fast framebuffer, replaces IOCS text)
8. Custom MFP UART driver (for serial debug, replaces IOCS serial)

### Phase F — Human68k Binary Compatibility (stretch goal)

1. X-format (.x) binary loader (header parse, relocations)
2. F-line exception handler (vector 11) — decode $FFxx DOS call opcodes
3. DOS call → PPAP syscall translation table (file I/O, process mgmt)
4. Argument translation (Human68k stack convention → PPAP registers)
5. Test with simple Human68k executables (e.g., command-line tools)

---

## 9. Risk Assessment

| Risk | Impact | Mitigation |
|---|---|---|
| Endianness bugs in filesystem code | Data corruption | Add `le16()`/`le32()` early; unit test on host with both endianness |
| QEMU m68k machine quirks | Stalled bringup | Start with simplest machine; have fallback to `mcf5208evb` |
| 68000 vs ColdFire ISA differences | Wrong instructions | Use `-m68000` flag strictly; avoid ColdFire-only instructions |
| PCB layout change breaks ARM | Regression | Phase A refactors first; CI runs all targets before m68k code lands |
| X68000 peripheral complexity | Scope creep | Defer X68000 until QEMU target is fully working |
| musl m68k maturity | Build issues | musl has upstream m68k support; well-tested in Linux m68k community |

---

## 10. Open Questions

1. **QEMU machine choice**: `virt` vs `mcf5208evb` vs `an5206`? Need to check
   which machines `qemu-system-m68k` supports with usable UART.

2. **ColdFire vs pure 68000**: QEMU's m68k CPU defaults may be ColdFire
   (ISA-A/B/C). We want pure 68000 for X68000 compatibility. Verify QEMU
   `-cpu m68000` flag availability.

3. **romfs alignment**: ARM requires 4-byte alignment for Thumb fetch. 68000
   requires 2-byte alignment for word/long access. Current 4-byte alignment
   is sufficient.

4. **No XIP on X68000**: X68000 has ROM at 0xFE0000 but it's the IPL ROM.
   All user code runs from RAM. The exec model changes significantly:
   both .text and .data segments must be loaded into RAM (no
   flash_text_base optimization). However, romfs can still live in a
   contiguous RAM region loaded from floppy at boot, and code can
   execute directly from that region without per-process copying —
   effectively "XIP from RAM". Only writable .data/.bss needs
   per-process pages.

5. **Stack frame format**: 68000 pushes {SR, PC} (6 bytes), 68010+ pushes
   {SR, PC, format/vector} (8 bytes). Which to target? Start with 68000
   format for maximum compatibility.