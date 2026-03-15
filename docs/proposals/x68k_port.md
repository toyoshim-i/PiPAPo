# X68000 Target Port — Implementation Status

Porting PPAP to the Sharp X68000 (MC68000 @ 10 MHz).  The QEMU `qemu_m68k`
target already proves the m68k kernel works; this document describes the
X68000-specific boot sequence, ELF loading, and process model as implemented.

---

## 1. Goals and Scope

### 1.1 Primary Goal

Produce a bootable PPAP system on the Sharp X68000 that:

- Boots from a 5.25" floppy disk (1.2 MB) with a two-stage bootstrap.
- Provides a console on the built-in TVRAM display via IOCS calls,
  mirrored to the RS-232C serial port.
- Mounts a UFS root filesystem from a RAM image loaded by stage2.
- Runs the PPAP userland test suite (`runtests`) and passes it.
- Executes Human68k `.x` and `.r` binaries natively (no CPU emulation).
- Runs an interactive BusyBox shell on XEiJ emulator.

### 1.2 Extended Goals

- Boot from SCSI HDD (larger capacity, faster iteration).
- Replace IOCS console with a direct TVRAM driver for full tty semantics.
- SRAM real-time clock persistence.
- Reuse community-written X68000 device drivers.

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
| F-line (Human68k DOS) and TRAP #15 handlers | `src/arch/m68k/boot.S` | See 4.3 |
| RAM size detection | `src/arch/m68k/probe_ram.S` | 100% (set `RAM_END=0xC00000`) |
| 32-bit multiply/divide (no libgcc) | `src/arch/m68k/math.S` | 100% |
| Human68k subsystem bridge | `src/kernel/subsys/human68k_bridge.c` | 100% |
| X-format / R-format binary loader | `src/kernel/exec/exec_x68k.c` | 100% |
| UFS filesystem driver | `src/kernel/fs/ufs.c` | 100% |
| VFS, tmpfs, procfs, devfs | `src/kernel/fs/`, `src/kernel/vfs/` | 100% |
| m68k user-space toolchain | `tools/m68k-toolchain/` | 100% |

---

## 3. X68000 Hardware

### 3.1 CPU and Memory

| Item | Value |
|------|-------|
| CPU | MC68000 @ 10 MHz |
| Main RAM | 1 MB (minimum) -- 0x000000-0x0FFFFF |
| Extended RAM | Up to 11 MB optional -- 0x100000-0xBFFFFF |
| GVRAM | 1 MB graphics -- 0xC00000-0xCFFFFF |
| TVRAM | 512 KB text -- 0xE00000-0xE7FFFF |
| Sprite / PCG | 0xEB0000-0xEBFFFF |
| SRAM | 16 KB battery-backed -- 0xED0000-0xED3FFF |
| IPL ROM | 128 KB -- 0xFE0000-0xFFFFFF |

`probe_ram` must be bounded at `RAM_END=0xC00000` to avoid clobbering
GVRAM (no bus error; reads return pixel data, writes produce artefacts).

**SRAM at 0xED0000 is battery-backed and user-visible.** The kernel and
stage bootstrap must not write to SRAM under any circumstances.

### 3.2 Key Peripherals

| Address | Peripheral | PPAP Use |
|---------|-----------|----------|
| 0xE88001 | MFP (MC68901) -- odd bytes, 2B stride | **UART console + timer** |
| 0xE8A001 | RTC (Ricoh RP5C15) -- odd bytes | SRAM clock (extended) |
| 0xE94000 | FDC (uPD765A) | **Floppy block driver** |
| 0xE96020 | SCSI (MB89352A) | Extended goal |
| 0xE9A000 | PPI (uPD8255) | Keyboard (extended) |
| 0xFE0000 | IPL ROM | **IOCS calls throughout boot** |

All I/O registers are 8-bit, at odd addresses of a word-width bus.
Always use byte (`.b`) operations; a word access aliases two registers.

### 3.3 IOCS Availability and Reentrancy

The IPL ROM code at **0xFE0000-0xFFFFFF remains mapped for the entire
lifetime of the machine** -- it is not overlaid after reset.  The IPL ROM
sets up the vector table at 0x000000 in RAM with TRAP #15 (vector 47)
pointing to the IOCS handler inside the ROM.

**As long as PPAP does not overwrite vector 47, IOCS is always callable
via `trap #15`.**

**CRITICAL: IOCS calls are non-reentrant.**  Functions like `_B_PUTC`
internally lower the CPU IPL to allow VSYNC synchronisation.  If the MFP
Timer-C ISR fires during an IOCS call and re-enters IOCS (e.g., via the
scheduler calling a console function), the shared IOCS work area at
0x000400-0x0007FF is corrupted, causing an address error crash at ROM
0x00FF775E.

**Mitigation:** All IOCS calls in `uart_x68k.c` are wrapped with
`ipl7_save()`/`ipl7_restore()` to raise IPL to 7 before entry, preventing
timer ISR preemption.  The scheduler's input poll callback uses
`uart_rx_avail_hw()` which reads the MFP USART RSR register directly
(0xE8802B bit 7) instead of calling `_B_KEYSNS`.

IOCS calls used by PPAP kernel:

| IOCS function | d0 value | Purpose |
|--------------|----------|---------|
| `_B_PUTC` | 0x20 | Write one character (TTY + serial) |
| `_B_GETC` | 0x21 | Read one character (keyboard/serial) |
| `_B_LOCATE` | 0x17 | Set TVRAM cursor position |
| `_B_KEYSNS` | 0x1C | Keyboard status (non-blocking) |
| `_B_CLR_ST` | 0x22 | Clear entire screen |
| `_B_CLR_ED` | 0x23 | Clear from cursor to end of screen |
| `_B_CLR_AL` | 0x19 | Clear current line from cursor |
| `_B_READ` | 0x46 | Read floppy sectors (CHS addressing) |
| `_OUT232C` | 0x35 | Write one character to RS-232C serial |

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
TCDCR bits [6:4] = 0b111  ->  prescaler /200
TCDR             = 200     ->  4 000 000 / 200 / 200 = 100 Hz
```

MFP vectored interrupt mapping:

| Channel | Function | Vector = VR_base + channel |
|---------|----------|---------------------------|
| 5 | Timer-C | 0x40 + 5 = **69** (0x45) -- used for scheduler tick |
| 10 | USART RX Buffer Full | 0x40 + 10 = **74** (0x4A) -- keyboard input |

#### USART for Serial Console and Keyboard

The MFP USART serves dual purpose on the X68000:
- **RS-232C serial port** (directly via `_OUT232C` for output)
- **Keyboard input** via the USART RX interrupt (vector 74)

The IPL ROM's keyboard interrupt handler at vector 74 reads scan codes
from the USART and fills the IOCS keyboard buffer.  **This vector must be
preserved** -- silencing it disables keyboard input entirely.

---

## 4. Boot Sequence — Detailed Implementation

### 4.1 IPL ROM Behaviour

On power-on the CPU reads from the ROM overlay mapped at 0x000000.  The IPL:

1. Initialises hardware and sets up the RAM vector table at 0x000000
   (including `TRAP #15` -> IOCS handler in ROM at 0xFExxxx).
2. Reads the first 1 KB of floppy track 0 (sector 0) into 0x002000.
3. Unmaps the ROM overlay (RAM now visible at 0x000000; ROM remains at
   0xFE0000).
4. Jumps to 0x002000 (our **Stage1**).

At the moment Stage1 starts, the RAM vector table is fully functional and
IOCS is available.

### 4.2 Stage1 (`src/target/x68k/boot/stage1.S`)

**Location:** Sector 0 of floppy, loaded by IPL ROM to 0x002000.
**Size limit:** 1024 bytes (one floppy sector).

Stage1 contains a BPB (BIOS Parameter Block) at offset 2-35 for floppy
format identification by emulators and Gotek drives.

```
Stage1 execution flow:
  1. Set sp = 0x002000   (pre-decrement grows into free RAM 0x000400-0x001FFF)
  2. Save IOCS vector:   d5 = *(uint32_t *)0x0000BC  (vector 47 = TRAP #15)
  3. Diagnostic:          IOCS _B_PUTC "Pi" to TVRAM
  4. Read loop:           IOCS _B_READ sectors 1-3 -> 0x003000 (Stage2)
     - LBA-to-CHS conversion: cyl = lsec/16, head = (lsec%16)/8, sec = (lsec%16)%8+1
     - Uses d4-d7/a3 (preserved across _B_READ)
  5. Store IOCS vector:  *(uint32_t *)0x002FF0 = d5  (handoff to stage2)
  6. jmp 0x003000
```

The temporary stack grows downward from 0x002000 into the free RAM region
0x000400-0x001FFF.  **Interrupts must remain enabled** -- `_B_READ` waits
for FDC interrupts internally.

On read error, Stage1 prints "E" followed by the hex FDC status code and
halts (`stop #0x2700`).

### 4.3 Stage2 (`src/target/x68k/boot/stage2.c`)

**Location:** Sectors 1-3 of floppy, loaded by Stage1 to 0x003000.
**Size limit:** 3072 bytes (three floppy sectors).
**Language:** C (compiled with `-m68000 -nostdlib -ffreestanding`).

Stage2 implements a minimal UFS parser sufficient to load two files from
the floppy's UFS partition.

#### System work areas that must NOT be touched

```
$000000-$0003FF  CPU exception vectors
$000400-$0007FF  IOCS work area (cursor, screen config, font data ...)
$000800-$000FFF  OPM (YM2151) driver work area
$001000-$001FFF  FDC / SCSI / DMA driver work area
```

Stage2 uses 0x003C00 (immediately after its own code) as its 4 KB scratch
buffer, safely above all system work areas.

#### Execution flow

```
Stage2 execution flow:
  1. Read UFS superblock (block 0 -> BUF at 0x003C00)
  2. Diagnostic: IOCS _B_PUTC "PA" (after first _B_READ, not before)
  3. Validate UFS magic (0x55465331)
  4. Read inode table block -> BUF; extract root inode (inode 1)
  5. Walk root directory -> find "boot" directory inode
  6. Walk boot/ directory -> find "kernel" and "rootfs.ufs" inodes
  7. Load kernel to 0x006000 (KERNEL_LOAD_ADDR)
     - Direct blocks (up to 10 x 4 KB = 40 KB)
     - Single indirect block (for files > 40 KB)
  8. Load rootfs.ufs to ROOTFS_BASE (= __page_pool_start from kernel ELF)
  9. Write handoff record:
     - 0x002FF4: 'RAMD' magic (0x52414D44)
     - 0x002FF8: rootfs RAM address
     - 0x002FFC: rootfs size in bytes
 10. stage2_final():
     a. Raise IPL to 7 (mask all interrupts)
     b. Save IPL ROM handlers:
        - iocs_handler = *(uint32_t *)0x002FF0  (saved by stage1)
        - kbd_handler  = vector[74]  (MFP USART RX = keyboard)
     c. Copy 256 longwords from 0x006000 to 0x000000 (kernel vector table)
     d. Restore preserved vectors:
        - vector[47] = iocs_handler  (TRAP #15: IOCS dispatch)
        - vector[74] = kbd_handler   (MFP ch.10: USART RX buffer full)
     e. jmp 0x006400 (Reset_Handler)
```

The diagnostic banner is split across the boot chain: Stage1 prints "Pi",
Stage2 prints "PA", and the kernel prints "Po" + " booting..." to spell
"PiPAPo booting...".

#### ROOTFS_BASE calculation

`mkx68kimg.sh` reads `__page_pool_start` from the kernel ELF and passes
it via `-DROOTFS_BASE=0x...u` when compiling stage2.  This ensures the
rootfs image lands above the kernel BSS (which `Reset_Handler` zeros at
boot before the kernel can read the rootfs).

#### IOCS stack corruption workaround

The IOCS `_B_READ` handler uses the supervisor stack extensively for FDC
wait loops and interrupt handling.  If the `ufs_inode_t` struct lives on
the same stack, IOCS can corrupt it.  `load_file()` copies all needed
inode fields into local variables **before** any floppy reads to avoid
reading from corrupted memory.

### 4.4 Vector Patching Strategy

Stage2 performs a selective vector table replacement:

1. **Save** vector 47 (TRAP #15 / IOCS) from stage1's handoff at 0x002FF0.
2. **Save** vector 74 (MFP USART RX / keyboard) from the live vector table.
3. **Copy** all 256 entries (1 KB) from the kernel binary at 0x006000 to
   the RAM vector table at 0x000000.
4. **Restore** vector 47 (IOCS dispatch) and vector 74 (keyboard input).

After this step, all PPAP exception handlers are live while IOCS and
keyboard input remain functional.

The kernel's `target_early_init()` in `target_x68k.c` performs additional
patching:

- **Autovectors 25-30** (OPM, MFP, reserved, SCC/VSYNC, FDC, DMA) are
  set to `m68k_irq_ignore` (bare `rte`) to prevent halts from Default_Handler.
- **MFP vectors 64-79** are set to `m68k_irq_ignore`, **except vector 74**
  (keyboard) which is explicitly skipped to preserve the IPL handler.
- **Vector 69** (Timer-C) is later overwritten by `timer_init()` with
  `m68k_timer_isr` for the scheduler tick.

### 4.5 Kernel Startup (`Reset_Handler` at 0x006400)

After stage2 jumps to 0x006400, the standard m68k `Reset_Handler` in
`boot.S` executes:

1. Set SSP = `__stack_top` (0x006400, grows down into freed boot area).
2. Copy `.data` (no-op: `__data_load == __data_start` since all RAM).
3. Zero `.bss` (`__bss_start` to `__bss_end`).
4. Jump to `kmain()`.

`kmain()` calls `target_early_init()` which patches interrupt vectors and
prints the banner, then proceeds with MM init, VFS setup, and process
creation.

### 4.6 Kernel Link Address and Memory Layout

The X68000 target links the kernel at **0x006000** (linker script:
`src/target/x68k/x68k.ld`).

#### Memory map after stage2 handoff

```
Address Range       Size        Contents
0x000000-0x0003FF   1 KB        Active vector table (copied by stage2)
0x000400-0x005FFF   22.75 KB    Freed boot area -> page pool
0x006000-0x0063FF   1 KB        Kernel .vectors (reused as SSP headroom)
0x006400-~0x02xxxx  ~120 KB     Kernel .text + .rodata + .data + .bss
~0x02xxxx           (aligned)   __page_pool_start (rootfs.ufs loaded here)
~0x02xxxx+rootfs    ...         Page pool start (after rootfs reservation)
...                 ...         Free pages for user processes
0x0FFFFF            ...         End of 1 MB main RAM
0x100000-0xBFFFFF   (optional)  Extended RAM (up to 11 MB)
```

#### Supervisor stack

The linker script sets `__stack_top = 0x006400`.  The kernel SSP starts
there and grows downward through the .vectors region (0x006000-0x0063FF,
which was already copied to 0x000000 and is no longer needed) and into
the freed boot area (0x000400-0x005FFF), giving approximately **23.75 KB**
of supervisor stack space -- far more than needed.

#### Rootfs RAM image

Stage2 loads `boot/rootfs.ufs` to `__page_pool_start`.  The kernel
validates the UFS magic at that address and computes the size from the
superblock (`block_count * block_size`).  Pages underlying the rootfs
image are reserved via `page_alloc_at()` so the allocator never hands
them out.  The rootfs is mounted read-only as `/` via `flatblk`.

---

## 5. Floppy Disk Layout

### 5.1 Outer UFS Structure

The floppy is formatted as a two-level UFS image built by `mkx68kimg.sh`:

```
Offset     Size    Contents
0 KB       1 KB    Boot sector (Stage1 -- IPL ROM loads to 0x002000)
1 KB       3 KB    Stage2 UFS loader (loaded by Stage1 to 0x003000)
4 KB       ...     Outer UFS filesystem (contains boot/ and romfs files)
```

The outer UFS contains:
- `boot/kernel` -- flat binary kernel image
- `boot/rootfs.ufs` -- inner UFS image (the actual root filesystem)

### 5.2 Inner rootfs.ufs

The inner UFS image is built by `mkufs` with `BIG_ENDIAN` mode (m68k is
big-endian).  It contains the complete root filesystem:

```
/sbin/init              First userland process
/bin/sh -> busybox      Shell (symlink to BusyBox)
/bin/busybox            BusyBox multi-call binary
/bin/runtests           On-target test runner
/bin/test_*             Test binaries
/etc/profile            Shell profile (TERM=dumb, PS1, PATH)
```

#### Big-endian symlink byte-swap

`mkufs` pre-swaps `i_direct[]` bytes for big-endian mode so that
`write_inode()`'s `w32()` cancels out, preserving raw symlink target
bytes on disk without byte-order corruption.

### 5.3 Build Configuration

```cmake
ppap_generate_romfs(ppap_x68k BIG_ENDIAN
    OVERLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/romfs
    EXCLUDE_APPS hello trace pdb)
```

The `EXCLUDE_APPS` list keeps the image small for floppy capacity.
The `OVERLAY_DIR` adds x68k-specific files like `/etc/profile`.

### 5.4 Floppy geometry

X68000 2HD format: 77 tracks x 2 heads x 8 sectors x 1024 bytes
= **1,232 KB** total.  The `.xdf` image must declare this geometry
for emulators and Gotek drives to accept it.

---

## 6. ELF Binary Loader — Non-XIP Path

### 6.1 Overview

On the X68000 target, user binaries reside in a UFS filesystem (either
the rootfs RAM image or a floppy UFS).  Unlike ARM targets where `.text`
can execute in place from flash (XIP), all m68k binaries are loaded
entirely into RAM before execution.

The loader (`src/kernel/exec/exec.c`) handles this via a unified path:

```
if (vn->xip_addr == NULL) {
    // Non-XIP: allocate contiguous pages, read entire file to buffer
    file_buf = alloc_contiguous(file_pages);
    vn->mount->ops->read(vn, file_buf, file_size, 0);
    // ... format detection (Human68k .x, .r, CP/M .com, ELF) ...
    elf_buf = file_buf;   // text runs in-place from these RAM pages
}
```

### 6.2 ELF Loading Steps

For a native m68k ELF binary from a non-XIP filesystem:

1. **Read entire file** into contiguous RAM pages allocated from the page
   pool.  These pages are **not freed** -- the `.text` segment runs
   in-place from them, exactly as romfs XIP works (both are in RAM on m68k).

2. **Validate ELF header** (`elf_validate`): checks EI_MAG, EI_CLASS=32,
   EI_DATA=big-endian, e_machine=EM_68K.

3. **Extract PT_LOAD segments** (`elf_load_segments`): up to 4 segments.
   Identifies text (PF_X) and data (PF_W) segments.

4. **Compute text base**: `text_base = file_buf + text_seg->p_offset`
   (physical RAM address where text was loaded).

5. **Compute entry point**: `entry = text_base + e_entry - text_seg->p_vaddr`
   (relocates the ELF virtual address to the actual RAM location).

6. **Allocate data pages** (if data segment exists):
   - `data_pages = ceil(p_memsz / PAGE_SIZE)`
   - `sram_page = alloc_contiguous(data_pages)`
   - Copy initialised data from file buffer
   - Zero BSS region (`p_memsz - p_filesz`)

7. **Relocate GOT** (Global Offset Table):
   - Find `.got` section via `elf_find_got()`
   - GOT entries pointing into text (below `data_seg->p_vaddr`) are
     relocated by adding `text_base`
   - GOT entries pointing into data (>= `data_seg->p_vaddr`) are
     relocated to `sram_page + offset`
   - With `-msep-data`, the **a5 register** holds the GOT base address
     (set by the kernel, not recalculated by the binary)

8. **Apply relocations** (`apply_relocations`):
   - Processes `.rel.dyn` / `.rela.dyn` sections
   - Handles `R_68K_RELATIVE` (data segment only -- no text relocs)
   - Handles `R_68K_JMP_SLOT` (PLT GOT entries for dynamic calls)
   - Skips entries already handled by GOT relocation

### 6.3 GOT Base Register (a5)

The m68k toolchain with `-msep-data -fPIC` generates position-independent
code that accesses all global data through the GOT via register **a5**.
Text and data locations are fully independent -- there is no PC-relative
GOT lookup as on ARM.

The kernel sets a5 (slot 13 in the software register frame on the SSP)
during `do_execve`:

```c
if (got_sram_addr) {
    uint32_t *sw = (uint32_t *)(uintptr_t)p->sp;
    sw[13] = got_sram_addr;   // a5 in the register frame
}
p->got_base = got_sram_addr;  // saved for signal handler restoration
```

When a signal handler runs, `m68k_call_signal_handler()` must restore a5
to `p->got_base` so the handler can access its global data correctly.

---

## 7. User Stack vs Supervisor Stack — m68k Process Model

### 7.1 MC68000 Dual Stack Architecture

The MC68000 has two physically separate stack pointers:

- **SSP** (Supervisor Stack Pointer, a7 in supervisor mode): used by the
  kernel for exception handling, syscall dispatch, and context switch.
- **USP** (User Stack Pointer, a7 in user mode): used by user-space code.

The CPU automatically switches between SSP and USP based on the S bit in
the Status Register (SR).  On exception entry (trap, interrupt, bus error),
the CPU pushes SR + PC onto the **SSP** and sets S=1.  On RTE with the
saved SR having S=0, the CPU switches back to the **USP**.

### 7.2 Page Allocation

Each process gets **two separate stack pages**:

| Page | PCB field | Purpose |
|------|-----------|---------|
| `stack_page` | `p->stack_page` | **Kernel stack (SSP target)**: exception frames, context switch save area, syscall handling |
| `user_stack_page` | `p->user_stack_page` | **User stack (USP target)**: argc/argv/envp/auxv, user function calls, local variables |

Allocation order in `do_execve()`:

```c
// Pre-allocate stack (SSP) page first to avoid LIFO brk conflict
void *stack = page_alloc();         // -> p->stack_page
void *user_stack = page_alloc();    // -> p->user_stack_page
```

The stack page is allocated **before** data pages to prevent LIFO
page-pool conflicts that would block brk expansion.

### 7.3 Exception Frame Construction

`proc_setup_stack()` builds the initial exception frame on the **kernel
stack** (stack_page), regardless of user_sp:

```
Kernel stack (SSP) layout, built by proc_setup_stack():
  [SP+0 ]  d0          <- pcb_t.sp points here
  [SP+4 ]  d1
  [SP+8 ]  d2
  [SP+12]  d3
  [SP+16]  d4
  [SP+20]  d5
  [SP+24]  d6
  [SP+28]  d7
  [SP+32]  a0
  [SP+36]  a1
  [SP+40]  a2
  [SP+44]  a3
  [SP+48]  a4
  [SP+52]  a5          <- GOT base (slot 13, patched by do_execve)
  [SP+56]  a6
  [SP+60]  SR (16-bit)  <- SR_USER (S=0, IPL=0) -> switches to USP on RTE
  [SP+62]  PC (32-bit)  <- entry point
```

Total: 15 registers x 4 bytes + 2 (SR) + 4 (PC) = **66 bytes**.

The SR is set to `SR_USER` (S=0, IPL=0), so when `switch.S` executes
`movem.l %sp@+,%d0-%d7/%a0-%a6; rte`, the CPU pops all registers, then
pops SR+PC.  Since SR has S=0, the CPU switches to user mode and the
hardware stack pointer becomes the USP.

### 7.4 USP Setup

The user stack (USP) is set up in `do_execve()`:

```c
// Build argc/argv/envp/auxv at top of user_stack page
uint32_t stack_top = (uint32_t)user_stack + PAGE_SIZE;
uint32_t sp = stack_top;
// ... push argv strings, auxv, envp, argv pointers, argc ...
p->usp = sp;  // saved in PCB
```

The PCB's `usp` field is loaded into the hardware USP register by
`switch.S` during context restore:

```asm
move.l  %a0@(PCB_USP_OFFSET),%a1
move.l  %a1,%usp
```

### 7.5 Context Switch — SSP and USP

Both `m68k_trap1_handler` (cooperative yield) and `m68k_timer_isr`
(preemptive tick) save and restore both stack pointers:

```asm
| Save
move.l  %sp,%a0@(PCB_SP_OFFSET)      | save SSP
move.l  %usp,%a1
move.l  %a1,%a0@(PCB_USP_OFFSET)     | save USP

| ... sched_next() picks new process ...

| Restore
move.l  %a0@(PCB_SP_OFFSET),%sp      | restore SSP
move.l  %a0@(PCB_USP_OFFSET),%a1
move.l  %a1,%usp                      | restore USP
```

### 7.6 exec_pending Protection

During `do_execve()`, there is a critical window between constructing the
new process's exception frame and the trap return path restoring it.  If
the timer ISR fires in this window and performs a context switch, it would
save the kernel's SSP into `current->sp`, overwriting the carefully
constructed exec frame.

The solution uses an `exec_pending` flag:

```c
// In do_execve(), with interrupts disabled:
uint32_t irq_save = arch_irq_save();
proc_setup_stack(p, entry, m68k_user_sp);
p->usp = m68k_user_sp;
// Patch GOT base in frame...
if (p == current)
    exec_pending[0] = 1;      // protect the frame
arch_irq_restore(irq_save);
```

The timer ISR in `switch.S` checks this flag:

```asm
tst.l   exec_pending
bne.s   .Lno_switch            | skip context switch if exec in progress
```

The flag is cleared by `trap.S`'s `.Lexec_restore` path after the new
process context has been successfully restored.

---

## 8. Console Strategy

### 8.1 VT100-to-IOCS Escape Sequence Converter

X68000 IOCS `_B_PUTC` has its own escape sequence parser that is **not
VT100-compatible**.  Sending VT100 CSI sequences directly causes the IOCS
parser to enter a bad state, potentially crashing.

The console driver (`src/target/x68k/drivers/uart_x68k.c`) intercepts
all output through a VT100 converter (`vt_feed()`) that:

- Tracks cursor position in software (`cur_x`, `cur_y`)
- Converts VT100 CSI sequences to equivalent IOCS calls:

| VT100 Sequence | IOCS Equivalent |
|----------------|-----------------|
| ESC[nA | Cursor up n (`_B_LOCATE`) |
| ESC[nB | Cursor down n (`_B_LOCATE`) |
| ESC[nC | Cursor forward n (`_B_LOCATE`) |
| ESC[nD | Cursor back n (`_B_LOCATE`) |
| ESC[row;colH | Cursor position (`_B_LOCATE`) |
| ESC[2J | Clear screen (`_B_CLR_ST`) |
| ESC[0J | Clear to end (`_B_CLR_ED`) |
| ESC[0K | Clear to EOL (`_B_CLR_AL`) |
| ESC[...m | SGR (silently ignored) |
| ESC[?... | Private modes (silently ignored) |

### 8.2 Dual Console Output

The console follows the **pico1 mirror pattern**: two outputs active
simultaneously.

- **TVRAM** (TTY_DISPLAY): `uart_putc()` -> VT100 converter -> IOCS calls
- **Serial** (TTY_SERIAL): `uart_serial_putc()` -> IOCS `_OUT232C`
- **klog mirror**: all kernel log output also goes to serial

### 8.3 Crash-Safe Mode

When the crash handler runs after an IOCS fault, calling `_B_PUTC` again
would cause a double bus/address error (CPU halt).  The `uart_tvram_inhibit`
flag makes `uart_putc` a no-op for TVRAM; crash diagnostics go only via
the serial mirror (`_OUT232C`).

### 8.4 TERM=dumb for Shell Startup

BusyBox hush's `ask_terminal()` sends ESC[6n (cursor position query)
before `/etc/profile` is sourced.  Since the X68000 IOCS cannot respond
to this query, it causes garbage bytes on screen.  The init process sets
`TERM=dumb` in the default environment to suppress this behaviour.

---

## 9. Implementation Phases — Status

### Phase X-1: Target Skeleton and IOCS Console -- COMPLETE

- `src/target/x68k/CMakeLists.txt` with `RAM_END=0xC00000`
- `src/target/x68k/x68k.ld` linker script (ORIGIN = 0x006000)
- `target_x68k.c` with IOCS-based console
- VT100 escape sequence converter
- Crash-safe uart_tvram_inhibit mode

### Phase X-2: Timer (MFP Timer-C) -- COMPLETE

- `src/target/x68k/drivers/timer_x68k.c`: MFP Timer-C at 100 Hz
- Preemptive scheduling works
- Vector 69 (Timer-C) installed by `timer_init()`

### Phase X-3: Stage1/Stage2 Bootstrap and Floppy Image -- COMPLETE

- `src/target/x68k/boot/stage1.S`: IPL bootstrap with BPB
- `src/target/x68k/boot/stage2.c`: UFS kernel + rootfs loader in C
- `tools/mkx68kimg/mkx68kimg.sh`: floppy image build tool
- `tools/mkufs/mkufs.c`: UFS image creator with big-endian symlink support
- Rootfs loaded to RAM, mounted as `/` via flatblk
- Keyboard vector preservation (vector 74)
- IOCS reentrancy protection (IPL7 wrappers)
- Direct MFP USART RSR polling for input availability

### Phase X-4: Integration Tests on Emulator -- IN PROGRESS

- Shell launches and runs interactively on XEiJ emulator
- 19 tests pass on m68k (qemu_m68k)
- XEiJ serial-over-TCP for automated test capture (`scripts/run_xeij_tcp.sh`)
- Remaining: automated `runtests` on XEiJ

### Phase X-5: Real Hardware Bring-up -- PLANNED

### Phase X-6: SCSI HDD and Extended Features -- PLANNED

---

## 10. Build and Test

### 10.1 Build Commands

```sh
./scripts/run.sh --test qemu_m68k    # build + run m68k tests on QEMU
./scripts/run.sh --test qemu_arm     # build + run ARM tests on QEMU
./scripts/run.sh --build x68k        # build X68000 floppy image
./scripts/run.sh --run x68k          # build + launch XEiJ emulator
```

### 10.2 Build Pipeline

```
1. Build kernel        -> build/x68k/ppap_x68k (ELF)
2. Build stage1        -> build/x68k/stage1.bin
3. Build stage2        -> build/x68k/stage2.elf (with -DROOTFS_BASE)
4. Build userland      -> build/x68k/romfs_ppap_x68k/
5. mkufs (inner)       -> build/x68k/rootfs.ufs (big-endian)
6. mkx68kimg (outer)   -> build/x68k/ppap_x68k.xdf (bootable floppy)
```

### 10.3 Test Results

| Target | Tests Pass | Notes |
|--------|-----------|-------|
| qemu_m68k | 19 | Full suite |
| qemu_arm | 17 | h68k_dos + x68k skipped (m68k-only) |
| x68k (XEiJ) | -- | Shell interactive, runtests pending |

---

## 11. Key Files

```
src/target/x68k/
  CMakeLists.txt         Build rules (RAM_END, drivers, flags)
  x68k.ld                Linker script (ORIGIN = 0x006000)
  target_x68k.c          Target hooks, vector patching, rootfs mount
  boot/
    stage1.S             IPL bootstrap (sector 0, <= 1024 bytes)
    stage2.c             UFS kernel + rootfs loader (sectors 1-3)
  drivers/
    uart_x68k.c          IOCS console, VT100 converter, MFP polling
    timer_x68k.c         MFP Timer-C driver (100 Hz tick)
  romfs/
    etc/profile          Shell startup (TERM=dumb, PS1, PATH)

src/arch/m68k/
  boot.S                 Vector table, Reset_Handler, bus/address error
  switch.S               Context switch (TRAP #1 + timer ISR)
  trap.S                 TRAP #0 syscall handler, exec_restore
  m68k_common.c          Crash handler (with uart_tvram_inhibit)
  math.S                 32-bit multiply/divide (no libgcc)

src/kernel/exec/
  exec.c                 ELF loader (XIP + non-XIP, GOT relocation)

tools/
  mkx68kimg/mkx68kimg.sh Floppy image build tool
  mkufs/mkufs.c          UFS image creator (big-endian support)
```

---

## 12. Risks and Open Questions

### 12.1 Real Hardware Bring-up

| Risk | Mitigation |
|------|-----------|
| Baud rate mismatch | Re-read MFP crystal; try 9600/19200/38400 |
| MFP vector base != 0x40 | Read VR at early_init before assuming |
| RAM probe GVRAM artefacts | Confirm RAM_END guard |
| SRAM corruption | No code writes to 0xED0000-0xED3FFF |

### 12.2 Floppy Capacity

With `PPAP_ENABLE_ECPU=OFF`, `PPAP_ENABLE_CPM=OFF`, and excluded apps
(hello, trace, pdb), the current floppy image uses ~1068 of 1232 sectors,
leaving ~164 sectors (~164 KB) of headroom.

### 12.3 SCSI Support

SCSI HDD support would remove the floppy capacity constraint and enable
running with eCPU and CP/M enabled.  Requires implementing `scsi_x68k.c`
for the MB89352A SCSI controller.

---

## 13. Related Documentation

- [docs/targets/68000.md](../targets/68000.md) -- m68k architecture reference
- [docs/subsystems/human68k.md](../subsystems/human68k.md) -- Human68k subsystem design
- [docs/ecpu/m68k.md](../ecpu/m68k.md) -- eCPU m68k emulator
- [docs/archive/history/target-68000-plan.md](../archive/history/target-68000-plan.md) -- Original m68k planning document
