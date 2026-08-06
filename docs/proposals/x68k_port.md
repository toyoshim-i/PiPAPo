# X68000 Target Port — Status and Remaining Work

Porting PPAP to the Sharp X68000 (MC68000 @ 10 MHz).  The `qemu_m68k`
target already proves the m68k kernel; the `x68k` target boots that same
kernel on a real/emulated X68000.

The port boots to an interactive shell on both consoles, with a colour
TVRAM console (X-6, done).  The remaining work, in order, is:

1. Host the root filesystem on a **SCSI hard disk** so the full PPAP
   userland test suite can run — the `--test` userland overflows the
   1.2 MB floppy, exactly as on `pcxt`.
2. Bring the port up on **real hardware**.

This document tracks **port status and the work that remains**.  The
technical reference for everything already implemented lives in the
permanent docs:

- **X68000 hardware, IOCS, boot chain, console** —
  [targets/m68k.md §7](../targets/m68k.md#7-x68000-target-x68k)
- **m68k ELF loading / GOT / a5** —
  [targets/m68k.md §6](../targets/m68k.md#6-user-space-details)
- **m68k process model (dual stacks, initial frame, exec-restore, vfork)** —
  [kernel/context_switch.md](../kernel/context_switch.md#m68k)
- **On-disk UFS format and the x68k boot media** —
  [kernel/ufs.md](../kernel/ufs.md#x68k-boot-interop)
- **Test workflow (XEiJ serial-over-TCP)** —
  [getting_started/testing.md](../getting_started/testing.md#x68k-xeij-testing)

---

## 1. Goals and Scope

### 1.1 Primary Goal

A bootable PPAP system on the Sharp X68000 that:

- Boots from a 5.25" floppy (1.2 MB) with a two-stage bootstrap **and** from
  a SCSI hard disk, honouring the SRAM boot-device setting like a stock
  X68000 (§3.4).
- Provides a console on the built-in TVRAM display via IOCS (with color and
  correct text geometry), mirrored to the RS-232C serial port, a getty on
  each.
- Mounts a 44bsd UFS root filesystem live from the boot device — `iocs_blk`
  ("fd0") for floppy, `scsi_blk` ("sd0") for HDD.
- Runs the PPAP userland test suite (`runtests`) and passes it.  The full
  suite runs from the **HDD image** (the floppy hosts a reduced set that fits
  1.2 MB).
- Executes Human68k `.x`/`.r` binaries natively (no CPU emulation).
- Runs an interactive shell on the XEiJ emulator.

### 1.2 Extended Goals

- Replace the IOCS console with a direct TVRAM driver for full tty semantics.
- SRAM real-time clock persistence.
- Reuse community-written X68000 device drivers.
- Raw MB89352A SCSI controller driver — IOCS-independent, for machines
  without SCSI-in-ROM.  **Not** in the current plan (§3.3); the port relies
  on the IPL ROM's SCSI BIOS.

### 1.3 Out of Scope

- Graphics, VRAM, or sprite-layer drivers.
- Sound (YM2151, ADPCM).
- X68030 MMU or 68030-specific instructions.
- Network drivers.

---

## 2. Phase Status

| Phase | Scope | Status |
|-------|-------|--------|
| X-1 | Target skeleton + IOCS console (VT100 converter, crash-safe mode) | ✅ Complete |
| X-2 | MFP Timer-C @ 100 Hz, preemptive scheduling | ✅ Complete |
| X-3 | Stage1/Stage2 bootstrap, floppy image, vector preservation | ✅ Complete |
| X-4 | Interactive shell on both consoles (TVRAM/keyboard + RS-232C getty) | ✅ Complete |
| X-5 | Live IOCS floppy block device (single 44bsd UFS, no in-RAM rootfs) | ✅ Complete |
| X-6 | Enhanced TVRAM console — color (SGR) + native tty geometry; TVRAM/serial driver split | ✅ Complete |
| X-7 | SCSI HDD rootfs — read path (`scsi_blk`, IOCS `_S_READ`) | ✅ Complete |
| X-8 | SCSI-IPL boot (SCSIINROM record 1) + boot-medium rootfs select (HDD independently bootable) | ✅ Complete |
| X-9 | Writable rootfs (IOCS `_S_WRITE`) | ⬜ Planned |
| X-10 | Full userland test suite on the HDD image; recorded `runtests` pass | ⬜ Planned |
| X-11 | Real hardware bring-up | ⬜ Planned |

The boot chain, console, scheduler, ELF loader, Human68k subsystem, and
live floppy UFS rootfs all work on XEiJ; both consoles are interactive.
Sections 3.x below track the remaining work in the same order as the
planned phases above.

---

## 3. Remaining Work

### 3.1 Serial input / scheduler health — resolved, with follow-ups

Serial input works.  The freeze that made the getty unresponsive was a
**blocking IOCS call under the Timer-C mask**: `uart_getc()` sensed the
keyboard with the wrong IOCS function (`$04`) and fell into the blocking
`_B_KEYINP`, which spins in ROM with the scheduler tick masked — so
preemption, `nanosleep`, the idle thread, and the idle-driven serial RX poll
all stalled until a key was pressed.  Fixed by sensing with `$01`
(`_B_KEYSNS`) and never issuing a blocking IOCS call under the guard.  Input
is delivered by the idle poll (`_LOF232C` count → `_INP232C` read for serial;
`_B_KEYSNS` for the display keyboard), both gated behind a non-blocking IOCS
try-lock.  Root-cause detail in `inside_human.md` §13.7–13.8.

Open scheduler-health follow-ups (independent of the phases below):

- **Drop the Timer-C MFP mask from the IOCS guard.** The mask
  (`x68k_iocs_irq_begin()`) predates the IOCS kmutex; the kmutex already
  serializes all process-context IOCS, the tick ISR never enters IOCS, and
  `kmutex_lock` panics on IRQ-context use — so the extra MFP mask is likely
  redundant with the kmutex and is exactly what lets one blocking call freeze
  the scheduler.  Removing it also keeps the tick alive through long IOCS
  calls (floppy `_B_READ`, SCSI `_S_READ`).  Verify the tick still cannot
  re-enter IOCS, then drop it.  The block-device read paths share this guard.
  While here, audit every IOCS call site under the guard for other
  potentially-blocking calls (the invariant in `inside_human.md` §13.7
  forbids them).
- **Flaky `kill`-getty SIGBUS.** Killing the serial getty has intermittently
  raised SIGBUS in the m68k wake/signal path.  Re-check now that the
  scheduler is healthy; may share a cause with the (fixed) freeze or be
  independent.

### 3.2 Enhanced TVRAM console — color + geometry (Phase X-6) ✅

Done, verified on XEiJ (96×32 with the colour prompt rendering).  The TVRAM
console (`tvram_x68k.c`) now:

- **Colour (SGR).** Maps ANSI `ESC[...m` onto the X68000 text palette via IOCS
  `_B_COLOR` — foreground to the nearest of Sharp's four text-palette entries
  (black/cyan/yellow/white), `+4` emphasis for bold, `+8` reverse for inverse.
  The palette itself is left unchanged so native Human68k programs keep Sharp's
  defaults; background colours and 256-colour SGR are dropped.
- **Geometry.** Queries the real text size from IOCS `_B_CONSOL` at init (96×32
  on the standard screen, replacing the hardcoded 96×31) and drives both the
  converter's wrap/clamp and the display tty's `get_cols`/`get_rows`, so
  `TIOCGWINSZ` is correct.

The RS-232C serial path was split into its own driver (`serial_x68k.c`) at the
same time, leaving `tvram_x68k.c` as the TVRAM console.

### 3.3 SCSI HDD rootfs — read path (Phase X-7)

**Motivation.** The m68k kernel already compiles in every subsystem
`qemu_m68k` runs — Human68k, CP/M, S-OS, and the Z80 eCPU are enabled
per-arch in `cmake/user.cmake`, not per-target.  What x68k lacks is **room on
disk**: the userland is staged into a single 44bsd UFS, and on the 1.2 MB
floppy that UFS only fits after `NO_ROGUE` + `EXCLUDE_APPS hello trace pdb`.
A `--test` build adds the full test-binary set (plus subsystem companion
binaries), overflowing the floppy.  The fix mirrors the `pcxt` port: build
**both** a floppy and a larger **SCSI HDD** image from one staging tree, and
run the test suite from the HDD.

**Access approach — IOCS SCSI only.** XEiJ's `-model=Hybrid` (already used)
ships IPL ROM 1.6 with an internal SCSI BIOS (SCSIINROM at `$FC0000`), so a
SCSI block driver issues standard IOCS `_SCSIDRV` (`$F5`) calls via
`trap #15` — structurally identical to how `iocs_blk` issues `_B_READ` for
the floppy, and reusing the same IOCS guard.  **No raw MB89352A controller
driver is planned**; that would only be needed for machines without
SCSI-in-ROM (deferred, §1.2).

**IOCS `_SCSIDRV` ABI** (`d0.b = $F5`, `d1.l = subfunction`, `trap #15`;
source: [Data Crystal — X68k/SCSI](https://datacrystal.tcrf.net/wiki/X68k/SCSI)):

| Call | `d1` | Inputs | Return `d0.l` |
|------|------|--------|---------------|
| `_S_TESTUNIT` | `$24` | `d4.b` = SCSI ID | `0` iff device ready |
| `_S_READCAP` | `$25` | `d4.b` = ID, `a1` = 8-byte buffer | `<0` err; buffer = LBA count + block length |
| `_S_READ` | `$21` | `d2.l` = LBA, `d3.l` = count (1–255), `d4.b` = ID, `d5.l` = block-size code (**1 = 512 B**), `a1` = dest | `<0` err |
| `_S_WRITE` | `$22` | as `_S_READ`, `a1` = source | `<0` err |

Block-size code `1` makes one SCSI record == one PPAP 512-byte blkdev
sector.  The single-page blkdev contract bounds `count` ≤ 8, far under the
255-block limit, so each `scsi_blk` request is a single `_S_READ`/`_S_WRITE`
straight into the target page (no scratch buffer).  Runtime device size comes
from `_S_READCAP`; ID 0 is probed with `_S_TESTUNIT`.

**HDD image (`.hds`).** XEiJ's SCSI image is a raw 512-byte-record disk led
by a 512-byte device-init header: magic `"X68SCSI1"` at offset 0,
`bytesPerRecord` word at +8 (= 512), `diskEndRecord` long at +10.  Attach
with `-sc0=<path.hds>`; `-boot=sc0` boots it.  `mkx68kimg.sh` writes the
header at LBA 0 and the rootfs UFS after it; `scsi_blk` maps blkdev sector S
to LBA (base + S).  *(Exact header field encoding, the UFS base LBA, and
which SCSI ID `-sc0` maps to are to be re-confirmed against XEiJ `SPC.java`
and empirically before the builder is written — no guessing.)*

**Build matrix (mirrors `pcxt`).** `mkx68kimg.sh` stages the userland once
and emits **both** images:

- `ppap_x68k.xdf` — floppy (stage1/2 + UFS).  **Gracefully skipped** when the
  staging overflows the floppy UFS budget (as `scripts/mkpcimg.sh` does), so a
  `--test` build produces the HDD image only.
- `ppap_x68k.hds` — SCSI HDD (`X68SCSI1` header + UFS).  Always built.

Normal build → both images; `--test` build → HDD only.

**Phase X-7 step.** New `scsi_blk.c`/`.h` (IOCS `_S_READ`, read-only)
registered as "sd0"; `.hds` builder + floppy graceful-skip in
`mkx68kimg.sh`.  Because Phase X-5 already mounts through the generic
`blkdev_t` interface, `scsi_blk` is a drop-in alongside `iocs_blk`.  To
isolate the driver, **temporarily** hardcode `target_mount_rootfs()` to "sd0"
while still booting the kernel from the floppy; verify `ls /` reads from the
HDD UFS on XEiJ (`-boot=fd0 -sc0=…hds`).  This temp config does not yet
consult SRAM — that arrives in X-8.

### 3.4 SCSI-IPL boot (SCSIINROM record 1) + boot-medium rootfs select (Phase X-8)

The X68000 IPL ROM reads the boot-device word at SRAM `$ED0018` — `0x0000` =
STD (scan by priority), `0x9070`–`0x9370` = FDn, `0x8000`–`0x8F00` = SASI HDn,
`0xA000` = ROM (SCSIINROM / SCSI HDD), `0xB000` = RAM (XEiJ `XEiJ.java`
`smrParseBootDevice`).  `-boot=sc0` writes `$ED0018 = 0xA000` and the SCSI-ROM
handle to `$ED000C`, so a SCSI disk boots through the ROM SCSIINROM path.

**SCSIINROM handoff** (traced from `IPLROM30.DAT`, entry `ff935a`): reads disk
record 0 and verifies the `X68SCSI1` signature, reads record 1 (the IPL,
always at byte `0x400` regardless of the 256/512-byte record size) to
`0x2000`, requires the first byte to be `$60` (`BRA`), and `JSR`s `0x2000` with
`D4.b` = boot SCSI ID.  It does **not** read the partition table — record 1 is
self-sufficient, so no Human68k partition/FAT is needed.

**Boot chain.** The whole SCSI loader (`scsi_head.S` + the shared
`stage2_core.c` + the SCSI read backend `stage2_scsi.c`) fits in the single
1024-byte record the ROM loads, so — unlike the floppy's stage1→stage2 — SCSI
boots in one hop: `scsi_head.S` saves the IOCS vector, sets a stack, and calls
`stage2_main()`, which reads `/boot/kernel` from the SCSI UFS via IOCS
`_SCSIDRV` and boots it.  `stage2_core.c` (superblock/inode walk, kernel load,
vector copy) is shared with the floppy path; only the block-read backend
differs (`stage2_floppy.c` `_B_READ` vs `stage2_scsi.c` `_SCSIDRV`).

**`.hds` layout** (512-byte records, `scsi_layout.h`): record 0 = `X68SCSI1`
header, record 2 (byte `0x400`) = SCSI IPL loader, rootfs UFS at LBA
`SCSI_UFS_BASE`.  `mkx68kimg.sh` builds the loader and lays out this bootable
image; the same UFS is mounted live by `scsi_blk` at runtime.

**Boot-medium rootfs select.** Rather than re-reading `$ED0018` in the kernel
(ambiguous under STD/auto-boot, where it reads `0x0000`), each stage2 backend
records its own medium in the boot handoff (`boot_handoff.h`: `0x2FF4` magic +
`0x2FF8` device code) — floppy writes `fd0`, SCSI writes `sd0`.
`target_early_init()` reads it before the low-RAM handoff region is reclaimed
as pages; `target_mount_rootfs()` mounts the matching UFS, replacing the X-7
hardcode.  This still follows the SRAM (the ROM used `$ED0018` to choose which
medium's loader to run), and one kernel image roots correctly from either
source.  SRAM is never written (`0xED0000`–`0xED3FFF` is battery-backed).

Verified on XEiJ (headless): `-boot=sc0` boots fully from the HDD
(`boot: rootfs on sd0`) and `-boot=fd0` still boots the self-contained floppy
(`boot: rootfs on fd0`).

### 3.5 Writable rootfs (Phase X-9)

Both block drivers are read-only through X-8: `iocs_blk_write()` returns
`-EIO` and (initially) so does `scsi_blk_write()`.  Implement `scsi_blk`
`_S_WRITE` (same guard as the read path) to give a writable `/`, unlocking
`test_rw`, `test_ufs`, and on-disk tmpfs.  A floppy `_B_WRITE` path (same
guard + LBA→CHS as its read path) remains an option for a writable floppy
rootfs but is not required for the test goal.

### 3.6 Full test suite on HDD + recorded pass (Phase X-10)

`runtests` passes on `qemu_m68k` (25/25; shared kernel, native m68k `init`
path).  For x68k: drop `NO_ROGUE` / `EXCLUDE_APPS` for the HDD image, stage
all test + subsystem binaries, and wire init → `/bin/runtests`.  Boot
`-boot=sc0` and capture the exact `ALL TESTS PASSED` marker over the XEiJ
serial-over-TCP channel (headless: XEiJ under Xvfb, serial streamed from
TCP :54321 with a hard timeout).  See
[testing.md](../getting_started/testing.md#x68k-xeij-testing).

### 3.7 Real hardware bring-up (Phase X-11)

Verified only on emulators so far.  Risks to check on real hardware:

| Risk | Mitigation |
|------|-----------|
| Baud rate mismatch | Re-read MFP crystal; try 9600/19200/38400 |
| MFP vector base ≠ 0x40 | Read VR at `early_init` before assuming |
| RAM probe GVRAM artefacts | Confirm `RAM_END=0xC00000` guard holds |
| SRAM corruption | Ensure no code writes 0xED0000-0xED3FFF |
| No SCSI-in-ROM on target model | HDD path needs a SCSI-BIOS model (SUPER/XVI/030/Compact); SASI-only machines (EXPERT/PRO/ACE) would need the deferred raw MB89352A driver (§1.2) |

---

## 4. Build and Run

```sh
./scripts/run.sh --build x68k        # build floppy + HDD images
./scripts/run.sh --run x68k          # build + launch XEiJ (floppy boot)
./scripts/run.sh --test x68k         # build HDD image + run full suite on XEiJ
./scripts/run.sh --test qemu_m68k    # m68k regression suite (shared kernel)
```

Build pipeline (one staging tree → two images):

```
1. Build kernel     -> build/x68k/ppap_x68k.bin (flat binary)
2. Build userland   -> build/x68k/romfs_ppap_x68k/ (staging tree; the .romfs
                       section is discarded by x68k.ld — the tree is packed
                       into the UFS, not embedded in the kernel)
3. scripts/mkx68kimg.sh:
   a. Assemble stage1/stage2 (floppy IPL + SCSI-IPL variants)
   b. mkufs -B over staging + /boot/kernel -> one 44bsd UFS
   c. Floppy: stage1 + stage2 + UFS -> ppap_x68k.xdf  (skipped if UFS
      overflows the 1.2 MB budget, e.g. under --test)
   d. HDD:    X68SCSI1 header + UFS -> ppap_x68k.hds   (always)
```

Key files:

```
src/target/x68k/
  CMakeLists.txt                    Build rules (RAM_END, drivers, flags)
  kernel/core/x68k.ld               Linker script (ORIGIN = 0x006000)
  kernel/core/target_x68k.c         Target hooks, vector patching, rootfs mount
  boot/stage1.S                     Floppy IPL bootstrap (sector 0, <= 1024 B)
  boot/stage2.c                     44bsd UFS kernel loader; reads SRAM $ED0018
  kernel/vfs/driver/tvram_x68k.c    TVRAM console: VT100 converter, SGR colour, geometry
  kernel/vfs/driver/serial_x68k.c   RS-232C serial console (_OUT232C/_LOF232C/_INP232C)
  kernel/vfs/driver/iocs_blk.c      IOCS _B_READ floppy block device ("fd0")
  kernel/vfs/driver/scsi_blk.c      IOCS _S_READ/_S_WRITE SCSI block device ("sd0")  [planned]
  kernel/core/driver/timer_x68k.c   MFP Timer-C driver (100 Hz tick)
  romfs/etc/profile                 Shell startup (TERM=dumb, PS1, PATH)
scripts/mkx68kimg.sh                Floppy + HDD image build tool
tools/mkufs/mkufs.c                 UFS image creator (big-endian support)
```

---

## 5. Related Documentation

- [targets/m68k.md](../targets/m68k.md) — m68k architecture + X68000 target reference
- [kernel/context_switch.md](../kernel/context_switch.md) — m68k context switch / vfork
- [kernel/ufs.md](../kernel/ufs.md) — UFS on-disk format and x68k boot interop
- [getting_started/testing.md](../getting_started/testing.md) — test workflow
- [subsystems/human68k.md](../subsystems/human68k.md) — Human68k subsystem design
- [ecpu/m68k.md](../ecpu/m68k.md) — eCPU m68k emulator
- [Data Crystal — X68k/SCSI](https://datacrystal.tcrf.net/wiki/X68k/SCSI) — IOCS `_SCSIDRV` (`$F5`) call reference
- XEiJ `tools/xeij/xeij/SPC.java` — SCSI `.hds` device-init header format and `-sc0`/`-boot=sc0` handling
```
