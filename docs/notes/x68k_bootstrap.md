# X68000 Bootstrap — Hardening Plan

Cross-reference of PPAP's X68000 two-stage boot sequence against the IPL ROM
disassembly (`iplrom.dat`) and Human68k 3.02 reverse engineering
(`docs/private/inside_human.md`).

PPAP replaces Human68k on top of the IOCS ROM BIOS.  We own the CPU, exception
vectors, process model, and filesystem — but delegate hardware interrupt handling
and low-level I/O to ROM IOCS via preserved vectors and `TRAP #15`.

## Background: ROM State at Boot Sector Entry

When the ROM IPL jumps to our boot sector at 0x2000 (inside_human.md §2.10):

| Item                    | State                                        |
|-------------------------|----------------------------------------------|
| SSP                     | 0x2000 (set at reset, not adjusted)          |
| SR                      | 0x2000 (supervisor, IPL=0, interrupts ON)    |
| Exception vectors       | 256 entries at RAM 0x000000, all ROM handlers |
| IOCS dispatch table     | 224 function ptrs at RAM 0x0400–0x0783       |
| TRAP #15                | Vector 47 → 0xFF1ADC (IOCS dispatch)         |
| IOCS work area          | 0x0400–0x07FF (ROM-initialized)              |
| OPM work area           | 0x0800–0x0FFF (ROM-initialized)              |
| FDC work area           | 0x1000–0x1349 (ROM-initialized)              |
| MFP/keyboard/video      | Fully initialized, interrupts running        |

PPAP boot chain: stage1 (sector 0, 0x2000) → stage2 (sectors 1–3, 0x3000) →
kernel (0x6000).  `stage2_final()` copies the kernel vector table to address 0,
then selectively restores ROM vectors (autovectors 24–31, MFP 64–79, TRAP #15)
so IOCS remains callable.

---

## Fix 1: Preserve Vector 11 (F-line Subsystem Trap)

**Bug**: `target_early_init()` overwrites vectors 10–23 with `m68k_irq_ignore`,
destroying vector 11 (`m68k_fline_handler`) — PPAP's subsystem trap for
Human68k DOS call dispatch (`dc.w 0xFFxx`).

**File**: `src/target/x68k/target_x68k.c`, line 135

**Change**:

```c
// Before:
for (uint32_t v = 10u; v < 24u; v++)
    vt[v] = ignore;

// After:
for (uint32_t v = 10u; v < 24u; v++) {
    if (v == 11u) continue;   /* F-line: PPAP subsystem trap */
    vt[v] = ignore;
}
```

**Also update the comment block** (lines 118–134) to list vector 11 among
the preserved vectors:

```
 *   0-9:   SSP, Reset, Bus/Address error, etc. (kernel handlers)
 *   11:    F-line emulator (PPAP subsystem trap — Human68k bridge)
 *   24-31: Autovectors (IPL ROM — VSYNC, SCC, etc.)
 *   ...
```

**Test**: run `./scripts/run.sh --test qemu_m68k` — the `test_h68k_dos`
test exercises F-line DOS calls and should pass.

---

## Fix 2: Update Stale Comment in `target_early_init()`

**Issue**: the comment at lines 100–107 says "autovectors 25-30 … all point to
Default_Handler at this point."  This was true before `stage2_final()` was added
but is now wrong — `stage2_final()` preserves all ROM autovectors and MFP
vectors.

**File**: `src/target/x68k/target_x68k.c`, lines 100–107

**Change**: rewrite to reflect the actual state:

```c
    /* Patch unused exception vectors with m68k_irq_ignore BEFORE the first
     * IOCS call.
     *
     * stage2_final() copied the kernel's .vectors to 0x000000 and then
     * restored ROM handlers for autovectors (24-31), MFP vectors (64-79),
     * and TRAP #15 (47).  The remaining kernel vectors that are still
     * Default_Handler (stop #0x2700) could halt the CPU if an unexpected
     * exception fires — replace them with m68k_irq_ignore (bare rte).
     *
     * NOTE: timer_init() is called later in target_late_init() and installs
     * m68k_timer_isr at vt[69].  It must come AFTER this loop.
     */
```

---

## Fix 3 (Optional): Clarify Stage2 Memory Safety

**Issue**: the comment in `target_mount_rootfs()` (line 209) warns that "the IPL
IOCS _B_READ handler corrupts the 0x002000-0x003FFF region during floppy I/O."
However, the ROM IPL itself loads the boot sector to 0x2000 using `_B_READ`
without self-corruption, and our stage2 works in practice.

**File**: `src/target/x68k/target_x68k.c`, line 209

**Change**: refine the comment to be more precise:

```c
    /* The IPL IOCS _B_READ handler may use low RAM as scratch during floppy
     * I/O — the exact range is ROM-version-dependent.  The handoff record
     * at 0x002FF4-0x002FFC is not relied upon; rootfs address and size are
     * derived from __page_pool_start and the UFS superblock instead. */
```

No code change needed — the current workaround (deriving rootfs address from
`__page_pool_start` + UFS superblock) is already correct.

---

## Non-Issues (Confirmed Safe)

### Stage1 SP = 0x2000

The ROM IPL sets SSP=0x2000 at reset (§2.2) and uses it for all IOCS calls
including `_B_READ` before jumping to the boot sector.  Our stage1 inherits
the same SP.  If this were unsafe, the machine would never boot.

### IOCS Work Area 0x07FC

The ROM writes a default value to 0x07FC during init (§2.5) but **never reads
it** (confirmed by exhaustive binary search of `iplrom.dat`).  Only Human68k
reads 0x07FC.  Since PPAP replaces Human68k, this address is unused.

### NMI During Vector Copy

`stage2_final()` masks interrupts during the vector copy, but NMI (level 7)
cannot be masked.  In practice, NMI only fires from the power-fail detect or
reset button — negligible risk.

### IOCS Calls From Idle Thread

Already mitigated: input polling uses direct hardware register reads
(`uart_rx_avail_hw()` at 0x0812, `uart_serial_rx_avail_hw()` at SCC RR0).
No IOCS TRAP #15 calls from idle or interrupt context.

---

## Step-by-Step Plan

### Step 1: Fix vector 11 preservation

1. Edit `src/target/x68k/target_x68k.c`:
   - Add `if (v == 11u) continue;` to the first vector patching loop
   - Update the comment block listing preserved vectors to include vector 11
2. Rewrite the stale comment (lines 100–107) per Fix 2
3. Build and test:
   ```sh
   ./scripts/run.sh --test qemu_m68k
   ```
4. Verify `test_h68k_dos` passes (exercises F-line DOS calls)

### Step 2: Refine stage2 memory comment

1. Edit `src/target/x68k/target_x68k.c`:
   - Update the `_B_READ` corruption comment in `target_mount_rootfs()` per Fix 3
2. No functional change — comment-only

### Step 3: Commit

Commit both fixes together as a single bootstrap hardening commit.
