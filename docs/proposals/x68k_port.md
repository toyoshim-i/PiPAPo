# X68000 Target Port Plan

Porting PPAP to the Sharp X68000 (MC68000 @ 10 MHz).  The QEMU `qemu_m68k`
target already proves the m68k kernel works; this plan describes the
additional work required to boot and run on real X68000 hardware and its
emulators (XM6 TypeG, XEiJ).

---

## 1. Goals and Scope

### 1.1 Primary Goal

Produce a bootable PPAP system on the Sharp X68000 that:

- Boots from a 5.25" floppy disk (1.2 MB) with a two-stage bootstrap.
- Provides a console on the built-in TVRAM display via IOCS calls from
  the first boot message, mirrored to the RS-232C serial port (same
  pattern as the pico1 target's USB + UART mirror).
- Mounts the boot floppy as a UFS volume and executes PPAP userland
  from it (no embedded romfs in the kernel binary).
- Runs the PPAP userland test suite (`runtests`) and passes it.
- Executes Human68k `.x` and `.r` binaries natively (no CPU emulation).

### 1.2 Extended Goals

- Boot from SCSI HDD (larger capacity, faster iteration).
- Replace IOCS console with a direct TVRAM driver for full tty semantics.
- Keyboard input via PPI (8255) for interactive use.
- SRAM real-time clock persistence.
- Reuse community-written X68000 device drivers (e.g., optimised FDC,
  FPU emulation library) to replace or supplement PPAP's own drivers.

### 1.3 Out of Scope

- Graphics, VRAM, or sprite-layer drivers.
- Sound (YM2151, ADPCM).
- X68030 MMU or 68030-specific instructions.
- Network drivers.

---

## 2. What Already Exists

| Component | File(s) | Reuse |
|-----------|---------|-------|
| m68k kernel core | `src/kernel/` | 100% |
| 68000 vector table and reset handler | `src/arch/m68k/boot.S` | 100% |
| Context switch (TRAP #1 + timer ISR) | `src/arch/m68k/switch.S` | 100% |
| TRAP #0 syscall handler | `src/arch/m68k/trap.S` | 100% |
| F-line (Human68k DOS) and TRAP #15 handlers | `src/arch/m68k/boot.S` | See §4.3 |
| RAM size detection | `src/arch/m68k/probe_ram.S` | 100% (set `RAM_END=0xC00000`) |
| 32-bit multiply/divide (no libgcc) | `src/arch/m68k/math.S` | 100% |
| Human68k subsystem bridge | `src/kernel/subsys/human68k_bridge.c` | 100% |
| X-format / R-format binary loader | `src/kernel/exec/exec_x68k.c` | 100% |
| UFS filesystem driver | `src/kernel/fs/ufs.c` | 100% |
| VFS, tmpfs, procfs, devfs | `src/kernel/fs/`, `src/kernel/vfs/` | 100% |
| m68k user-space toolchain | `tools/m68k-toolchain/` | 100% |

The dominant new work is: stage1 and stage2 bootstrap assembly, MFP UART
driver, MFP Timer-C driver, floppy image build tooling, and a linker script.

---

## 3. X68000 Hardware

### 3.1 CPU and Memory

| Item | Value |
|------|-------|
| CPU | MC68000 @ 10 MHz |
| Main RAM | 1 MB (minimum) — 0x000000–0x0FFFFF |
| Extended RAM | Up to 11 MB optional — 0x100000–0xBFFFFF |
| GVRAM | 1 MB graphics — 0xC00000–0xCFFFFF |
| TVRAM | 512 KB text — 0xE00000–0xE7FFFF |
| Sprite / PCG | 0xEB0000–0xEBFFFF |
| SRAM | 16 KB battery-backed — 0xED0000–0xED3FFF |
| IPL ROM | 128 KB — 0xFE0000–0xFFFFFF |

`probe_ram` must be bounded at `RAM_END=0xC00000` to avoid clobbering
GVRAM (no bus error; reads return pixel data, writes produce artefacts).

**SRAM at 0xED0000 is battery-backed and user-visible.** The kernel and
stage bootstrap must not write to SRAM under any circumstances.

### 3.2 Key Peripherals

| Address | Peripheral | PPAP Use |
|---------|-----------|----------|
| 0xE88001 | MFP (MC68901) — odd bytes, 2B stride | **UART console + timer** |
| 0xE8A001 | RTC (Ricoh RP5C15) — odd bytes | SRAM clock (extended) |
| 0xE94000 | FDC (µPD765A) | **Floppy block driver** |
| 0xE96020 | SCSI (MB89352A) | Extended goal |
| 0xE9A000 | PPI (µPD8255) | Keyboard (extended) |
| 0xFE0000 | IPL ROM | **IOCS calls throughout boot** |

All I/O registers are 8-bit, at odd addresses of a word-width bus.
Always use byte (`.b`) operations; a word access aliases two registers.

### 3.3 IOCS Availability

The IPL ROM code at **0xFE0000–0xFFFFFF remains mapped for the entire
lifetime of the machine** — it is not overlaid after reset. The IPL ROM
sets up the vector table at 0x000000 in RAM with TRAP #15 (vector 47)
pointing to the IOCS handler inside the ROM.

**As long as PPAP does not overwrite vector 47, IOCS is always callable
via `trap #15`.**  This means PPAP never needs to reimplement IOCS for
kernel use — it simply calls it directly, exactly as Human68k programs do.

IOCS calls used by PPAP kernel:

| IOCS function | d0 value | Purpose |
|--------------|----------|---------|
| `_B_PUTC` | 0x20 | Write one character (TTY + serial) |
| `_B_GETC` | 0x21 | Read one character (keyboard/serial) |
| `_B_LOCATE` | 0x11 | Set TVRAM cursor position |
| `_B_PUTMES` | 0x1A | Write string to TVRAM |
| `_B_KEYSNS` | 0x1C | Keyboard status (non-blocking) |
| `_B_READ` | 0x06 | Read floppy sectors (CHS addressing) |
| `_B_WRITE` | 0x07 | Write floppy sectors |

IOCS calls are non-reentrant and must be guarded with a spinlock when
called from process context if interrupts are enabled.

### 3.4 MFP (MC68901) Register Map

Base: 0xE88000; register N at `0xE88000 + 1 + 2*N`.

| N | Register | Purpose |
|---|----------|---------|
| 3 | IERA | Interrupt enable A |
| 4 | IERB | Interrupt enable B |
| 11 | VR | Vector base register (IPL ROM sets to 0x40) |
| 12 | TACR | Timer A control |
| 14 | TCDCR | Timers C and D control |
| 17 | TCDR | Timer C count |
| 20 | UCR | USART control |
| 21 | RSR | Receiver status |
| 22 | TSR | Transmitter status |
| 23 | UDR | USART data |

#### Timer-C for 100 Hz

MFP clock input on X68000: 4 MHz.

```
TCDCR bits [6:4] = 0b111  →  prescaler ÷200
TCDR             = 200     →  4 000 000 / 200 / 200 = 100 Hz
```

The MFP generates a vectored interrupt (not autovectored).  The IPL ROM
sets `VR = 0x40`, giving Timer-C vector `0x40 + 10 = 0x4A` (= 74 decimal,
address `0x000128` in the vector table).  The ISR body in `switch.S` is
reused verbatim; only the vector table entry at 0x000128 needs updating.

#### USART for Serial Console

The MFP USART drives the RS-232C port at the back of the X68000.  Exact
baud-rate prescaler value depends on the hardware revision; 9600 bps is
the safe default.  Polling TSR[7] (transmit buffer empty) and RSR[7]
(receive buffer full) is sufficient for Phase X-1.

---

## 4. Boot Sequence

### 4.1 IPL ROM Behaviour

On power-on the CPU reads from the ROM overlay mapped at 0x000000.  The IPL:

1. Initialises hardware and sets up the RAM vector table at 0x000000
   (including `TRAP #15` → IOCS handler in ROM at 0xFExxxx).
2. Reads the first 1 KB of floppy track 0 (sector 0) into 0x002000.
3. Unmaps the ROM overlay (RAM now visible at 0x000000; ROM remains at
   0xFE0000).
4. Jumps to 0x002000 (our **Stage1**).

At the moment Stage1 starts, the RAM vector table is fully functional and
IOCS is available.

### 4.2 Stage1 (1 KB, sector 0 of the floppy — loaded to 0x002000)

Stage1 uses only 68000 register arithmetic and IOCS calls.  It must fit
in exactly 1024 bytes.

```
Stage1 tasks:
  1. Set sp = 0x002000   (stage1's own load address; first push pre-decrements
                          to 0x1FFC, growing down into free RAM 0x000400–0x001FFF)
  2. IOCS _B_READ sectors 1-3 → load 3 KB to 0x003000  (Stage2)
  3. jmp 0x003000
```

The temporary stack grows downward from 0x002000 into the free RAM region
0x000400–0x001FFF, which is empty after the IPL runs.  This region is later
freed to the page pool by the kernel MM init.  Setting SP to the stage1 load
address itself is conventional on 68000: the stack never touches the stage1
code because pre-decrement guarantees the first write goes to 0x1FFC.

### 4.3 Stage2 (up to 3 KB, sectors 1–3 — loaded to 0x003000)

Stage2 has enough code budget for a minimal UFS parser.  It:

1. Sets an independent stack (reuses the temporary stack; SP = 0x002000,
   growing down into 0x000400–0x001FFF, same as Stage1).
2. Opens the UFS partition on the floppy (starts at physical sector 4;
   see §5 for disk layout).
3. Reads the UFS superblock (block 0 of the partition = sector 4–7).
4. Finds the `/boot/kernel` inode by walking the root directory.
5. Reads the kernel binary sequentially into RAM starting at **0x006000**
   (see §4.5 for link address rationale).
6. Patches the RAM vector table in place (see §4.4).
7. Jumps to `Reset_Handler` at 0x006000 + 0x400.

Stage2 does not call IOCS from inside a vector handler, so locking is
not needed at this point.

### 4.4 Vector Patching Strategy

PPAP does **not** wholesale-replace the vector table.  Stage2 performs a
selective patch:

1. Save the current value of vector 47 (TRAP #15 / IOCS):
   `iocs_vec ← *(uint32_t *)0x0000BC`

2. Copy **all 256 entries** (1 KB) from the kernel's embedded vector
   table (at the very start of the kernel binary at 0x006000) into the
   RAM vector table at 0x000000.

3. Restore vector 47 to `iocs_vec` (preserves the IPL IOCS handler).

4. Inspect which other IPL vectors should be preserved (open question —
   see §9.3).  Candidates to keep from IPL: TRAP #9 (Trace), level-1–5
   and level-7 autovectors, TRAP #2–#14 if the IPL provides useful
   handlers.

After this, all PPAP exception handlers are live, and IOCS remains
functional for the kernel and for Human68k user-space programs.

### 4.5 Kernel Link Address

The X68000 target links the kernel at **0x006000** (unlike `qemu_m68k`
which links at 0x000000).  This removes any risk of stage1/stage2
overwriting the kernel during load, and gives a clear low-memory layout.

Justification for 0x006000 rather than a smaller address:

| Address range | Size | Reservation |
|---------------|------|-------------|
| 0x000000–0x0003FF | 1 KB | Vector table (active, permanent) |
| 0x000400–0x001FFF | 7.5 KB | Temporary boot stack (SP=0x002000, grows down here); freed to page pool |
| 0x002000–0x0023FF | 1 KB | Stage1 (freed to page pool after jump) |
| 0x002400–0x002FFF | 3 KB | Stage1–Stage2 gap + stack (freed) |
| 0x003000–0x003BFF | 3 KB | Stage2 code (freed after kernel starts) |
| 0x003C00–0x005FFF | 8.5 KB | Gap; freed to page pool |
| **0x006000–0x0063FF** | **1 KB** | Kernel vector table in binary (freed after patching) |
| 0x006400–end | — | Kernel `Reset_Handler`, text, data, BSS |

After `kmain()` initialises MM, the freed early-stage regions are put to
direct use rather than just handed to the general page pool:

**0x006000–0x0063FF (1 KB) → kernel supervisor stack**

The x68k linker script sets `__stack_top = 0x006400`.  The kernel SSP
starts there and grows down into 0x006000–0x0063FF — the 1 KB that used
to hold the in-binary copy of the vector table, which was already patched
into RAM at 0x000000 by stage2 and is no longer needed.  This dedicates
a naturally-placed 1 KB region to the supervisor stack without consuming
any page-pool pages.

**0x000400–0x005FFF (22.75 KB) → first page-pool pages**

These pages are registered as free pages first during MM init.  Because
they are at the lowest addresses in the pool, the kernel's early
fixed-size allocations (process table, vnode table, fd table, pipe
buffers, kmalloc slab) draw from them before touching higher RAM.  On a
1 MB machine this is material: the kernel's static data structures
typically consume 10–30 KB, so they fit entirely within the reclaimed
boot area, leaving the upper RAM almost entirely available for user
process pages.

**Summary for a 1 MB machine:**

| Region | Size | Final use |
|--------|------|-----------|
| 0x000000–0x0003FF | 1 KB | Active vector table (reserved) |
| 0x000400–0x005FFF | 22.75 KB | Page pool — kernel data structures |
| 0x006000–0x0063FF | 1 KB | Kernel supervisor stack (`__stack_top`) |
| 0x006400–~0x030000 | ~170 KB | Kernel text + data + BSS |
| ~0x030000–0x0FFFFF | ~820 KB | Page pool — user process pages |

With a typical 80–150 KB kernel binary the page pool (kernel data +
user pages combined) covers roughly 840–900 KB = 210–225 pages on a
1 MB machine.

---

## 5. Floppy Disk Layout

### 5.1 Format Choice

The X68000 floppy format: 77 tracks × 2 heads × 8 sectors × 1024 bytes
= **1,232 KB** total capacity.  The boot floppy is formatted as **PPAP UFS**
(already implemented in `src/kernel/fs/ufs.c`), with the first 4 KB
(sectors 0–3) reserved as a boot area outside the UFS partition.

```
Offset     Size    Contents
0 KB       1 KB    Boot sector (Stage1 — IPL ROM loads to 0x002000)
1 KB       3 KB    Stage2 UFS loader (loaded by Stage1 to 0x003000)
4 KB       4 KB    UFS superblock (block 0 of UFS partition)
8 KB       4 KB    UFS block bitmap (block 1)
12 KB      4 KB    UFS inode bitmap (block 2)
16 KB      N×4KB   UFS inode table (block 3 … N+2)
(N+3)×4KB  rest    UFS data blocks (kernel, init, utilities, …)
```

The UFS block size is 4 KB (matching the existing `UFS_BLOCK_SIZE`
constant).  When the kernel mounts the floppy, it passes a **partition
offset of 4 KB** to the block device, so the UFS driver sees its "block 0"
at physical byte 4096 on the disk.  The block device abstraction already
carries a sector-offset field for SD-card partitions; the same mechanism
applies here.

### 5.2 No Embedded romfs

The X68000 target is built **without** the baked-in romfs that other
targets carry.  All userland binaries and data files reside on the UFS
floppy.  The kernel binary itself (linked at 0x006000) contains only the
kernel text, data, and BSS.  This reduces the kernel binary to roughly
100–200 KB, well within floppy capacity.

Configuration flags for the x68k build:

```cmake
PPAP_ENABLE_ROMFS=OFF       # No embedded romfs
PPAP_ENABLE_ECPU=OFF        # ecpu-m68k not needed (native execution)
PPAP_ENABLE_CPM=OFF         # CP/M subsystem optional (add to secondary disk)
```

### 5.3 Disk Contents — Primary Floppy (boot disk)

Essential files only.  Build-time configured.

```
/boot/kernel           PPAP kernel binary (loaded by Stage2)
/bin/init              First userland process
/bin/sh                Minimal shell
/bin/runtests          On-target test runner
/bin/test_exec         }
/bin/test_vfork        } Core tests (small binaries)
/bin/test_pipe         }
/bin/test_signal       }
/bin/test_x68k         } Human68k binary execution test
/bin/test_h68k_dos     }
/dev, /proc, /tmp      Synthesised by devfs / procfs / tmpfs at mount time
```

### 5.4 Disk Contents — Secondary Floppy (/mnt/fd2)

Extended tests, tools, and optional subsystems.

```
/bin/test_*            Remaining on-target tests (test_cpm, test_trace, …)
/bin/pdb               Debugger
/subsys/human68k/      Sample Human68k binaries
/subsys/cpm/           CP/M binaries (if PPAP_ENABLE_CPM=ON)
```

### 5.5 VFS Mount Strategy

On boot the kernel:

1. Mounts the boot floppy as UFS at `/mnt/fd1` (read-write).
2. Binds (or symlinks) key subdirectories into the namespace:

```
/bin  →  /mnt/fd1/bin
/lib  →  /mnt/fd1/lib
/etc  →  /mnt/fd1/etc
```

The binding mechanism (symlink vs. `vfs_bind`-style remount) is an open
design question noted in §9.4.

When the user inserts the secondary floppy and requests a mount, the
kernel mounts it at `/mnt/fd2`.  Binaries on the secondary disk are
accessed via their full path or via a `/mnt/fd2/bin` entry in `PATH`.

The same VFS pattern applies to the planned VFAT SD-card support on
RP2040 — the x68k UFS floppy is essentially the same concept, making
both implementations converge naturally.

---

## 6. Console Strategy

The console follows the **pico1 mirror pattern**: two outputs active
simultaneously from the first boot message.

### 6.1 Phase X-1 and X-2 — IOCS-based Console (MVP)

Both outputs are driven by IOCS calls, no custom driver:

- **TVRAM** via `_B_PUTMES` / `_B_LOCATE` — text appears on the built-in
  CRT immediately after the IPL ROM finishes.
- **Serial** via `_B_PUTC` — characters also appear on an attached
  terminal or PC running minicom / TeraTerm at 9600 bps.

`console_putc()` calls both IOCS functions.  `console_getc()` polls both
`_B_KEYSNS` (keyboard) and `_B_GETC` (serial) and returns the first
available character.

This requires no driver code beyond a thin wrapper — IOCS handles all the
display and USART complexity.

### 6.2 Phase X-4 — Direct TVRAM Driver (replaces IOCS for display)

Once the kernel is stable, add a direct TVRAM driver:

- Writes characters directly to TVRAM at 0xE00000 (font data from the
  system font ROM, or a small embedded font).
- Provides full tty semantics (scrolling, cursor, ANSI colour optionally).
- Registered as `/dev/tty0` (primary console).
- UART (MFP USART) registered as `/dev/ttyS0`; console output mirrored
  there as on pico1.

### 6.3 When to Switch

Keep the IOCS-based console until Phase X-3 passes (runtests on emulator).
Switch to the direct TVRAM driver in Phase X-4 as a clearly isolated
improvement without blocking integration tests.

---

## 7. New Files and Directory Layout

```
src/arch/m68k/x68k/
  stage1.S              — IPL bootstrap (≤1024 bytes)
  stage2.S              — UFS kernel loader (≤3 KB, 68000 assembly + C)

src/target/x68k/
  CMakeLists.txt        — build rules (RAM_END, link addr, drivers, flags)
  x68k.ld               — linker script (ORIGIN = 0x006000, no romfs)
  target_x68k.c         — target hooks: early_init, console_init, late_init
  drivers/
    uart_x68k.c         — MFP USART driver (IOCS wrapper initially; direct later)
    timer_x68k.c        — MFP Timer-C driver (100 Hz tick)
    fdc_x68k.c          — µPD765A floppy block driver (for UFS mount)
    scsi_x68k.c         — MB89352A SCSI (Phase X-6)

tools/
  mkx68kfloppy/
    mkx68kfloppy.c      — produces .XDF floppy image:
                          boot area (stage1 + stage2) + UFS image
```

Changes to existing files:

| File | Change |
|------|--------|
| `scripts/build.sh` | Add `x68k` as valid target |
| `scripts/run.sh` | Add `x68k` target (runs under XEiJ) |
| `src/kernel/blkdev/blkdev.h` | Verify `start_offset` field exists for partition support |

---

## 8. Implementation Phases

### Phase X-1: Target Skeleton and IOCS Console

**Goal**: "PiPAPo booting... [x68k]" on TVRAM and serial.

1. Create `src/target/x68k/CMakeLists.txt` with `RAM_END=0xC00000`,
   link origin 0x006000, `PPAP_ENABLE_ROMFS=OFF`, `PPAP_ENABLE_ECPU=OFF`.

2. Write `x68k.ld` (copy `qemu_m68k.ld`, change `ORIGIN` to 0x006000,
   remove romfs section).

3. Write `target_x68k.c`:
   - `early_init()`: IOCS-based `console_putc` wrapper (calls `_B_PUTMES`
     and `_B_PUTC`).  Print banner.
   - `late_init()`: mount floppy UFS as root; start init process.

4. Verify the kernel binary builds and is < 200 KB (no romfs).

5. Load the kernel manually into XEiJ at 0x006000 via its debugger.
   Patch vectors manually in XEiJ (or have a minimal stage2 stub).
   Observe "PiPAPo booting..." in the XEiJ TVRAM and serial log windows.

**Verification**: Kernel banner visible on XEiJ, no crash during early_init.

---

### Phase X-2: Timer (MFP Timer-C)

**Goal**: Preemptive scheduler; `sleep` and `waitpid` timeout work.

1. Write `timer_x68k.c`:
   - `timer_init()`: Read VR from MFP.  Set TCDCR[6:4]=0b111 and TCDR=200.
     Enable Timer-C interrupt in IERB and IMRB.
     Register `m68k_timer_isr` at vector `VR+10` in the RAM vector table.
   - The timer ISR body in `switch.S` is reused verbatim (handles tick +
     context switch identically to the Goldfish RTC ISR on QEMU).

2. Verify interrupt fires at ~100 Hz using a busy-loop tick counter.

3. Run `test_time` and `test_sleep_intr` to confirm timing.

**Verification**: `test_time` elapsed values within 10% of expected.

---

### Phase X-3: Stage1/Stage2 Bootstrap and Floppy Image

**Goal**: X68000 boots PPAP entirely from a floppy image.

1. Write `src/arch/m68k/x68k/stage1.S` (≤1024 bytes):
   ```
   | stage1.S — loaded by IPL ROM to RAM:0x002000
   set  sp = 0x002000              | pre-decrement grows stack into 0x1FFC…
   IOCS _B_READ sectors 1-3       | load 3 KB to 0x003000
   jmp  0x003000                  | hand off to stage2
   ```

2. Write `src/arch/m68k/x68k/stage2.S` (≤3 KB, C-level with asm glue):
   ```
   | stage2 — UFS kernel loader at 0x003000
   a. Set sp = 0x002000              | same pre-decrement stack as stage1
   b. IOCS _B_READ sectors 4-7   → superblock (block 0 of UFS partition)
   c. Parse superblock: find inode table location, data block start
   d. Read root inode (inode 1)
   e. Walk root directory to find "boot/kernel"
   f. Read kernel inode; read data blocks sequentially to 0x006000
   g. Patch vectors:
      iocs_vec = *(u32*)0x0000BC           | save TRAP #15
      memcpy(0x000000, 0x006000, 1024)     | copy kernel vector table
      *(u32*)0x0000BC = iocs_vec           | restore IOCS
   h. jmp 0x006400                         | jump to Reset_Handler
   ```

3. Write `tools/mkx68kfloppy/mkx68kfloppy.c`:
   - Reads `stage1.bin`, `stage2.bin`, and a pre-built UFS image.
   - Produces a `.XDF` file:
     - Bytes 0–1023: stage1 binary (padded to 1 KB)
     - Bytes 1024–4095: stage2 binary (padded to 3 KB)
     - Bytes 4096–end: UFS image
   - Validates total size ≤ 1,232 KB.

4. Extend `scripts/build.sh x68k` to:
   - Build kernel (no romfs); produce `ppap_x68k.bin` (flat binary from ELF).
   - Run `mkufs` to create `ppap_x68k.ufs` containing the kernel and
     essential userland binaries.
   - Run `mkx68kfloppy` to produce `ppap_x68k.xdf`.

5. Load `ppap_x68k.xdf` into XEiJ (File → FDD A → Open).  Boot and
   observe PPAP booting without manual intervention.

**Verification**: PPAP boots and reaches userland from the XDF image.

---

### Phase X-4: Integration Tests on Emulator

**Goal**: `runtests` passes on XEiJ with the floppy image.

1. Verify the UFS floppy mounts correctly as root; all test binaries
   accessible at `/bin/test_*`.

2. Run `runtests`.  Expected results:
   - `test_x68k`, `test_h68k_dos`: PASS (m68k native Human68k execution)
   - `test_cpm`: SKIP (disabled in initial build)
   - All other core tests: PASS

3. Add `--test x68k` path to `run.sh` that launches XEiJ in batch mode
   (XEiJ supports `-boot` argument) or documents the manual procedure.

4. Optionally: switch TVRAM output from IOCS-based to direct TVRAM driver
   (Phase X-4b) as a self-contained follow-up.

**Verification**: "ALL TESTS PASSED" in XEiJ serial log.

---

### Phase X-5: Real Hardware Bring-up

**Goal**: PPAP boots on a physical X68000.

Expected failure modes and mitigations:

| Failure | Mitigation |
|---------|-----------|
| Baud rate mismatch | Re-read MFP crystal assumption; try 19200 / 38400 |
| MFP vector base ≠ 0x40 | Read VR from RAM at early_init before assuming 0x40 |
| RAM probe GVRAM artefacts | Confirm RAM_END guard activates before probe loop |
| Stage2 CHS wrap bug | Careful floppy track/sector advance logic in Stage2 |
| SRAM corruption | Confirm no code writes to 0xED0000–0xED3FFF |

**Verification**: "ALL TESTS PASSED" over RS-232C serial port on real hardware.

---

### Phase X-6: SCSI HDD and Extended Features (Extended Goals)

#### SCSI Driver (MB89352A)

Implement `scsi_x68k.c` with SCSI-1 READ/WRITE commands.  Mount a UFS
partition from the SCSI disk.  This removes the floppy capacity constraint
and allows running with eCPU and CP/M enabled.

#### Community Driver Reuse

The X68000 community has published standalone drivers and libraries (IPL
IOCS replacements, FPU emulation, FDD optimisation) in Human68k .x / .r
format.  If PPAP can load and call these as kernel modules or as userland
helpers, it gains:

- Higher-performance FDD I/O (bypasses IOCS overhead).
- FPU emulation for 68000 software that uses 68881 instructions.

This is architecturally feasible because PPAP already executes Human68k
`.x` binaries.  A thin `dlopen`-like mechanism for loading a `.x` binary
as a kernel-side driver (with restricted syscall access) could bridge the
two worlds.  This is entirely post-Phase-X-5 work.

#### Full TVRAM tty and Keyboard

Direct TVRAM driver + PPI keyboard gives a fully interactive console
without needing a serial terminal.

---

## 9. Risks and Open Questions

### 9.1 Stage2 Budget (3 KB)

A minimal UFS parser in 68000 assembly requires: superblock read (one
4 KB IOCS read), inode table read, directory scan, sequential file read.
Estimated code size: 1.5–2.5 KB.  This fits in the 3 KB budget with
care.  If it overflows, extend to sectors 1–6 (6 KB) — the IPL ROM only
enforces that sector 0 is the boot sector; larger secondary loader areas
are conventional.

### 9.2 Floppy Image Tooling

**Risk**: The `.XDF` file must declare correct geometry (77 tracks, 2 heads,
8 sectors × 1 KB) in its header.  Incorrect geometry causes emulators and
Gotek drives to reject the image silently.

**Mitigation**: Validate the `.XDF` in XEiJ and XM6 TypeG before declaring
Phase X-3 done.  The Gotek + HxC firmware also supports `.XDF` with correct
headers.

### 9.3 Which IPL Vectors to Preserve

Beyond TRAP #15 (IOCS), other IPL ROM vectors may be useful:

| Vector | Description | Preserve? |
|--------|-------------|-----------|
| 9 | Trace | No — PPAP does not implement trace exception |
| 10 | A-line | No — PPAP has Default_Handler |
| TRAP #2–#14 | Various IPL handlers | Review per ROM revision |
| Level 1–5, 7 autovectors | Peripheral IRQs | No — PPAP uses Default_Handler for now |
| Level 6 autovector | Timer (overridden by MFP) | No — replaced by our timer ISR |

The safe default: preserve **only TRAP #15**.  If any feature needs an
additional IPL vector (e.g., TRAP #14 for error recovery), add it
explicitly.

### 9.4 VFS Namespace Binding

The mechanism to bind `/bin` → `/mnt/fd1/bin` at boot is unresolved.
Options:

1. **Symlinks**: `/bin` is a symlink to `/mnt/fd1/bin`.  Requires tmpfs
   at `/` or a writable overlay.  Works if init process creates them.

2. **`vfs_bind`**: A new VFS operation that grafts a sub-tree from one
   mount onto another path.  Similar to Linux `--bind` mounts.  Cleaner
   but requires new VFS code.

3. **PATH env var**: No binding.  Programs are invoked with full paths
   (`/mnt/fd1/bin/sh`) or PATH includes `/mnt/fd1/bin`.  Simplest short
   term.

4. **Floppy root**: Mount the floppy directly as `/` (not `/mnt/fd1`).
   Requires the UFS driver to support being the root mount.  Most similar
   to how romfs works today.  Requires careful boot sequencing (kernel
   needs to mount UFS before starting init).

Option 4 most closely mirrors the existing `qemu_m68k` / RP2040 approach
where romfs is `/`.  This is the recommended starting point: boot floppy
is mounted as `/`.  Secondary floppy is mounted at `/mnt/fd2`.  A
subdirectory structure on the primary floppy provides `/bin`, `/lib`,
etc. directly.

### 9.5 Kernel Binary Size vs. Floppy Capacity

With PPAP_ENABLE_ECPU=OFF, PPAP_ENABLE_CPM=OFF, and no romfs:
estimated kernel binary ≈ 80–150 KB.  This leaves 1,000–1,100 KB for the
UFS filesystem including userland binaries.  The core test binaries
(test_exec, test_vfork, test_pipe, etc.) are each < 20 KB.  The full
primary-disk userland (init, sh, runtests, ~15 test binaries) should fit
within 400 KB, leaving ample headroom.  If the kernel grows beyond
estimate, ecpu or the trace subsystem can be disabled incrementally.

### 9.6 UFS Partition Offset Support

The existing UFS driver reads "block 0" as the superblock.  For the
X68000 floppy, the UFS superblock is at byte offset 4096 (after the boot
area).  Verify that the block-device abstraction already supports a
`start_offset` (in blocks or bytes) and that the UFS mount call passes
this correctly; if not, add it as a prerequisite.

---

## 10. Dependency Graph

```
X-1 (console + kernel build)
  └─→ X-2 (MFP timer)
        └─→ X-3 (stage1/2 + floppy image + UFS mount)
              └─→ X-4 (emulator integration tests)
                    └─→ X-5 (real hardware)
                          └─→ X-6 (SCSI + TVRAM tty + community drivers)
```

X-1 and X-2 can be developed in parallel; X-2 only needs X-1 for
diagnostic output.  X-3 requires the UFS driver to work (already done)
and the `blkdev` partition-offset support (§9.6).

---

## 11. Related Documentation

- [docs/targets/68000.md](../targets/68000.md) — m68k architecture reference and hardware overview
- [docs/subsystems/human68k.md](../subsystems/human68k.md) — Human68k subsystem design
- [docs/ecpu/m68k.md](../ecpu/m68k.md) — eCPU m68k emulator
- [docs/archive/history/target-68000-plan.md](../archive/history/target-68000-plan.md) — Original m68k planning document
