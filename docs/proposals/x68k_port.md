# X68000 Target Port — Status and Remaining Work

Porting PPAP to the Sharp X68000 (MC68000 @ 10 MHz).  The `qemu_m68k`
target already proves the m68k kernel; the `x68k` target boots that same
kernel from a real/emulated X68000 floppy.

This document tracks **port status and the work that remains**.  The
technical reference for everything already implemented now lives in the
permanent docs:

- **X68000 hardware, IOCS, boot chain, console** —
  [targets/m68k.md §7](../targets/m68k.md#7-x68000-target-x68k)
- **m68k ELF loading / GOT / a5** —
  [targets/m68k.md §6](../targets/m68k.md#6-user-space-details)
- **m68k process model (dual stacks, initial frame, exec-restore, vfork)** —
  [kernel/context_switch.md](../kernel/context_switch.md#m68k)
- **On-disk UFS format and the x68k boot floppy** —
  [kernel/ufs.md](../kernel/ufs.md#x68k-boot-interop)
- **Test workflow (XEiJ serial-over-TCP)** —
  [getting_started/testing.md](../getting_started/testing.md#x68k-xeij-testing)

---

## 1. Goals and Scope

### 1.1 Primary Goal

A bootable PPAP system on the Sharp X68000 that:

- Boots from a 5.25" floppy (1.2 MB) with a two-stage bootstrap.
- Provides a console on the built-in TVRAM display via IOCS, mirrored to
  the RS-232C serial port.
- Mounts a 44bsd UFS root filesystem live from the floppy via `iocs_blk`
  ("fd0").
- Runs the PPAP userland test suite (`runtests`) and passes it.
- Executes Human68k `.x`/`.r` binaries natively (no CPU emulation).
- Runs an interactive shell on the XEiJ emulator.

### 1.2 Extended Goals

- Boot from SCSI HDD (larger capacity, faster iteration).
- Replace the IOCS console with a direct TVRAM driver for full tty semantics.
- SRAM real-time clock persistence.
- Reuse community-written X68000 device drivers.

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
| X-4 | Integration tests on XEiJ | 🟡 In progress |
| X-5 | Live IOCS block device (single 44bsd UFS, no in-RAM rootfs) | ✅ Complete |
| X-6 | Real hardware bring-up | ⬜ Planned |
| X-7 | SCSI HDD and extended features | ⬜ Planned |

The boot chain, console, scheduler, ELF loader, Human68k subsystem, and
live UFS rootfs all work on XEiJ.  Boot reaches scheduler startup, `init
started`, and the serial getty prompt.  See the implementation references
linked at the top.

---

## 3. Remaining Work

### 3.1 UART RX / serial input (Phase X-4)

Serial input now works.  The freeze that made the getty unresponsive was a
**blocking IOCS call under the Timer-C mask**: `uart_getc()` sensed the
keyboard with the wrong IOCS function (`$04`) and fell into the blocking
`_B_KEYINP`, which spins in ROM with the scheduler tick masked — so
preemption, `nanosleep`, the idle thread, and the idle-driven serial RX poll
all stalled until a key was pressed.  Fixed by sensing with `$01`
(`_B_KEYSNS`) and never issuing a blocking IOCS call under the guard.  Input
is delivered by the idle poll (`_LOF232C` count → `_INP232C` read for serial;
`_B_KEYSNS` for the display keyboard), both gated behind a non-blocking IOCS
try-lock.  Root-cause detail in `inside_human.md` §13.7–13.8.

Remaining sub-items:

- **Drop the Timer-C MFP mask from the IOCS guard.** The mask
  (`x68k_iocs_irq_begin()`) predates the IOCS kmutex; the kmutex already
  serializes all process-context IOCS, the tick ISR never enters IOCS, and
  `kmutex_lock` panics on IRQ-context use — so the extra MFP mask is likely
  redundant with the kmutex and is exactly what lets one blocking call freeze
  the scheduler.  Removing it also keeps the tick alive through long IOCS
  calls (floppy `_B_READ`).  Verify the tick still cannot re-enter IOCS, then
  drop it.  Note §3.4's `_B_WRITE` path shares this guard.  While here, audit
  every IOCS call site under the guard for other potentially-blocking calls
  (the invariant in `inside_human.md` §13.7 forbids them).
- **Flaky `kill`-getty SIGBUS.** Killing the serial getty has intermittently
  raised SIGBUS in the m68k wake/signal path.  Re-check now that the
  scheduler is healthy; may share a cause with the (fixed) freeze or be
  independent.
- **Recorded pass + real-hardware confirmation.** Both consoles are
  interactive on XEiJ — the TVRAM/keyboard shell and the serial getty (the
  latter verified headlessly).  What remains is an automated recorded pass
  (§3.6) and real-hardware bring-up (§3.7).

### 3.2 TVRAM color (SGR) escape support

The VT100→IOCS converter in `uart_x68k.c` currently **silently ignores**
`ESC[...m` (SGR) sequences, so colored output (e.g. `ls`, the shell prompt,
rogue) renders monochrome on the TVRAM display.  X68000 TVRAM is a color
text plane, so the converter can map SGR to IOCS text-color attributes:

- Map foreground (30-37) and background (40-47) codes to the X68000 text
  palette, plus reset (0), bold/bright (1), and reverse (7).
- Apply the color via the appropriate IOCS call when emitting subsequent
  characters, tracking current attributes in software like `cur_x`/`cur_y`.
- Keep unsupported SGR parameters silently ignored (no parser wedge).

The serial mirror (`_OUT232C`) already passes raw bytes through, so this
only affects the TVRAM path.  See the SGR row in
[targets/m68k.md §7.7](../targets/m68k.md#77-console-strategy).

### 3.3 TVRAM text-area size / tty geometry

The console does not yet reflect the X68000's native TVRAM text dimensions
in the tty.  The VT100 converter tracks `cur_x`/`cur_y` in software, and the
tty window size (rows × cols reported via `TIOCGWINSZ`) must match the
actual TVRAM text area — otherwise full-screen apps (rogue) and the shell's
line editing wrap, scroll, and clear at the wrong boundaries.

- Query the active TVRAM text geometry (via IOCS / CRTC mode) at console
  init instead of assuming a fixed size.
- Set the tty window size from that, so `TIOCGWINSZ` and the converter's
  cursor-clamp / line-wrap / clear math use the real screen geometry.
- Re-derive on any text-mode change so the two stay in sync.

### 3.4 Writable rootfs

`iocs_blk` currently implements only the `_B_READ` path;
`iocs_blk_write()` returns `-EIO`, so the rootfs is effectively read-only.
Wiring the IOCS `_B_WRITE` path (with the same guard + Timer-C masking and
LBA→CHS conversion as the read path) makes the floppy rootfs writable.

### 3.5 SCSI HDD support (Phase X-7)

A SCSI HDD removes the floppy capacity constraint and allows building with
the eCPU and CP/M subsystems enabled.  It requires a `scsi_blk` driver for
the MB89352A controller (0xE96020).  Because Phase X-5 already mounts the
rootfs through the generic `blkdev_t` interface, the SCSI backend is a
drop-in: it registers under the same API the kernel already mounts against,
alongside or instead of `iocs_blk`.

### 3.6 Automated testing on XEiJ (Phase X-4)

`runtests` runs and passes on `qemu_m68k` (25/25, the shared kernel and
native m68k `init` path), but is **not yet wired** to run automatically on
XEiJ.  Remaining: drive `/bin/runtests` over the XEiJ serial-over-TCP
channel and capture an `ALL TESTS PASSED` marker.  See
[testing.md](../getting_started/testing.md#x68k-xeij-testing).

### 3.7 Real hardware bring-up (Phase X-6)

Verified only on emulators so far.  Risks to check on real hardware:

| Risk | Mitigation |
|------|-----------|
| Baud rate mismatch | Re-read MFP crystal; try 9600/19200/38400 |
| MFP vector base ≠ 0x40 | Read VR at `early_init` before assuming |
| RAM probe GVRAM artefacts | Confirm `RAM_END=0xC00000` guard holds |
| SRAM corruption | Ensure no code writes 0xED0000-0xED3FFF |

### 3.8 Floppy capacity

With `PPAP_ENABLE_ECPU=OFF`, `PPAP_ENABLE_CPM=OFF`, and excluded apps
(hello, trace, pdb), the image uses ~1068 of 1232 sectors (~164 KB
headroom).  Enabling the eCPU/CP/M subsystems needs either tighter trimming
or the SCSI HDD of §3.5.

---

## 4. Build and Run

```sh
./scripts/run.sh --build x68k        # build the X68000 floppy image (.xdf)
./scripts/run.sh --run x68k          # build + launch XEiJ
./scripts/run.sh --test qemu_m68k    # m68k regression suite (shared kernel)
```

Build pipeline:

```
1. Build kernel        -> build/x68k/ppap_x68k.bin (flat binary)
2. Build userland      -> build/x68k/romfs_ppap_x68k/ (staging tree)
3. scripts/mkx68kimg.sh:
   a. Assemble stage1  -> stage1.bin (sector 0)
   b. Compile stage2   -> stage2.bin (sectors 1-3)
   c. mkufs -B over romfs staging + /boot/kernel -> one 44bsd UFS
   d. Concatenate stages + UFS -> build/x68k/ppap_x68k.xdf
```

Key files:

```
src/target/x68k/
  CMakeLists.txt                    Build rules (RAM_END, drivers, flags)
  kernel/core/x68k.ld               Linker script (ORIGIN = 0x006000)
  kernel/core/target_x68k.c         Target hooks, vector patching, rootfs mount
  boot/stage1.S                     IPL bootstrap (sector 0, <= 1024 B)
  boot/stage2.c                     44bsd UFS kernel loader (sectors 1-3)
  kernel/vfs/driver/uart_x68k.c     IOCS console, VT100 converter, MFP polling
  kernel/vfs/driver/iocs_blk.c      IOCS _B_READ block device ("fd0")
  drivers/timer_x68k.c              MFP Timer-C driver (100 Hz tick)
  romfs/etc/profile                 Shell startup (TERM=dumb, PS1, PATH)
scripts/mkx68kimg.sh                Floppy image build tool
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
