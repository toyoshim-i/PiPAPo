# Semihosting TTY Backend

Add an ARM semihosting backend as an additional serial I/O channel, activated
by a build flag. When enabled, the debugger (QEMU or OpenOCD) can serve as a
secondary console alongside the existing hardware UART, which remains the
primary default.

---

## 1. Motivation

### Problem

PPAP currently relies on a hardware UART for all serial I/O:

- **QEMU ARM** — CMSDK UART at `0x40004000` (emulated peripheral)
- **Pico 1/2** — PL011 UART at `0x40034000` (physical GPIO 0/1)

This works but has limitations:

1. **QEMU testing** — The mps2-an500 CMSDK UART requires `-serial mon:stdio`
   and has unreliable SysTick TICKINT. Semihosting is QEMU's native I/O
   mechanism and avoids peripheral emulation quirks entirely.
2. **Hardware debugging** — When debugging Pico hardware via OpenOCD/SWD,
   semihosting can provide console output without a separate USB-serial
   adapter. Useful for one-cable debugging setups.
3. **New target bringup** — When porting to a new board, semihosting gives
   immediate printf capability before any peripheral driver is written.

### Goal

Add a `PPAP_SEMIHOST` build flag that provides ARM semihosting as an
additional serial I/O channel alongside the existing UART. The UART remains
the primary default. The TTY layer, line discipline, signals, and all
user-space I/O work unchanged.

---

## 2. ARM Semihosting Primer

ARM semihosting is a debug-channel protocol defined by ARM. The target CPU
executes a special instruction; the debugger (QEMU, OpenOCD, J-Link) traps it
and performs the requested I/O on the host.

### Trap Instruction

| CPU | Instruction | Notes |
|-----|------------|-------|
| Cortex-M (Thumb) | `bkpt 0xAB` | ARMv6-M, ARMv7-M, ARMv8-M |
| ARM (A32) | `svc 0x123456` | Not used by PPAP |
| RISC-V | 3-instruction `ebreak` sequence | Future (Hazard3 on RP2350) |

On entry: `r0` = operation number, `r1` = pointer to argument block.

### Operations Used by PPAP

| Op | Number | Arguments | Description |
|----|--------|-----------|-------------|
| SYS_WRITEC | 0x03 | `r1` = pointer to char | Write one character |
| SYS_WRITE0 | 0x04 | `r1` = pointer to NUL-terminated string | Write string |
| SYS_WRITE | 0x05 | `r1` = `{fd, buf, len}` | Write buffer to file handle |
| SYS_READC | 0x07 | (none) | Read one character (blocks host) |
| SYS_READ | 0x06 | `r1` = `{fd, buf, len}` | Read from file handle |

For output, `SYS_WRITEC` (0x03) is simplest — one `bkpt` per character. For
bulk output, `SYS_WRITE` (0x05) with `fd=1` (stdout) is more efficient.

For input, `SYS_READC` (0x07) returns one character. On QEMU it returns
immediately (−1 if no input); on OpenOCD it may block.

### Host-Side Setup

**QEMU:**
```sh
qemu-system-arm -M mps2-an500 -serial mon:stdio -semihosting -nographic -kernel ppap.elf
```

`-semihosting` is added alongside `-serial mon:stdio` (not replacing it).
The CMSDK UART remains the default serial backend; semihosting is an
additional channel enabled by the build flag.

**OpenOCD:**
```
arm semihosting enable
arm semihosting_fileio enable
```

---

## 3. Design

### 3.1 Build Flag

```cmake
option(PPAP_SEMIHOST "Use ARM semihosting for serial I/O" OFF)
```

When `ON`:
- Compile `semihost.c` (new file) into the kernel
- Define `PPAP_SEMIHOST=1` as a compile definition
- Skip UART driver compilation (or keep it but don't wire it to TTY)

### 3.2 Architecture

```
User process
    │  write(1, buf, n)
    ▼
TTY layer (tty.c)                  ← unchanged
    │  tty_backend_t.putc()
    ▼
┌───────────────┐  ┌─────────────────────────┐
│ UART backend  │  │  semihost_backend       │  ← NEW (additional)
│ (default)     │  │  putc: bkpt 0xAB        │
│ uart_putc()   │  │  getc: bkpt 0xAB        │
└───────┬───────┘  └────────────┬────────────┘
        │                       │
        ▼                       ▼
  CMSDK / PL011         QEMU / OpenOCD / J-Link
  (hardware UART)       (host debugger)
```

The UART backend remains the default for TTY_SERIAL. The semihosting backend
can be wired as:
- A **klog mirror** via `klog_set_mirror()` (debugger sees kernel output)
- A **second TTY device** (e.g., `/dev/ttySH0`)
- A **TTY_SERIAL replacement** via `tty_set_backend()` (one-cable debug)

PPAP already has a pluggable backend via `tty_set_backend()`. The semihosting
backend just implements `tty_backend_t`.

### 3.3 Backend Implementation

```c
/* src/drivers/arch/arm_m/semihost.c */

static inline int semihost_call(int op, const void *arg)
{
    register int r0 __asm__("r0") = op;
    register const void *r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

static int semihost_putc(char c, void (*notify)(void))
{
    (void)notify;
    semihost_call(0x03 /* SYS_WRITEC */, &c);
    return 1;  /* semihosting is synchronous, never full */
}

static int semihost_getc(void)
{
    int ch = semihost_call(0x07 /* SYS_READC */, NULL);
    return (ch < 0) ? -1 : ch;
}

static int semihost_rx_avail(void)
{
    return 0;  /* polling-based: TTY layer will poll on read */
}

static const tty_backend_t semihost_backend = {
    .putc     = semihost_putc,
    .flush    = NULL,
    .getc     = semihost_getc,
    .rx_avail = semihost_rx_avail,
};
```

### 3.4 Wiring

The UART driver is always compiled and initialized — it remains the primary
serial backend. Semihosting is wired as an additional I/O channel:

1. **TTY backend** — register semihosting as a second TTY device (e.g.,
   `TTY_SEMIHOST`), or optionally replace `TTY_SERIAL` via
   `tty_set_backend(TTY_SERIAL, &semihost_backend)` in `target_late_init()`
   when semihosting-as-primary is desired (e.g., one-cable debug setups).

2. **Early klog** — `klog.c` calls `uart_putc()` directly for kernel logging.
   The UART driver handles this as before. When `PPAP_SEMIHOST` is enabled,
   klog can optionally mirror output to semihosting via `klog_set_mirror()`
   (already supported). This gives debugger-visible output without changing
   the primary UART path.

### 3.5 Input Considerations

Semihosting input (`SYS_READC`) behaves differently across hosts:

| Host | SYS_READC behavior |
|------|--------------------|
| QEMU | Returns immediately; −1 if no input |
| OpenOCD | Blocks until a character is available |
| J-Link | Blocks (with timeout) |

For **QEMU testing** (the primary use case), non-blocking `SYS_READC` works
well — the TTY read path already handles "no data available" by blocking the
process and retrying.

For **OpenOCD**, blocking `SYS_READC` would stall the CPU. Input support on
OpenOCD should be considered out of scope for the initial implementation —
output-only is sufficient for debugging.

### 3.6 Impact on Existing Targets

| Target | PPAP_SEMIHOST=OFF (default) | PPAP_SEMIHOST=ON |
|--------|-----------------------------|------------------|
| qemu_arm | CMSDK UART only | CMSDK UART **+ semihosting** (QEMU needs `-semihosting`) |
| pico1 | PL011 UART only | PL011 UART **+ semihosting** (OpenOCD) |
| pico1calc | PL011 UART only | PL011 UART **+ semihosting** (OpenOCD) |
| pico2 | PL011 UART only | PL011 UART **+ semihosting** (OpenOCD) |
| qemu_m68k | N/A | N/A (ARM-only feature) |

The flag is off by default. No change to any existing build or test workflow.

When `PPAP_SEMIHOST=ON`, the **existing UART driver is still compiled and
used as the primary serial backend**. The semihosting driver provides a
second TTY device (`/dev/ttyS1` or a dedicated `/dev/ttySH0`) that
applications can explicitly open, or can be selected as the klog output
for debugger-attached sessions.

Alternatively, the semihosting backend can replace the UART as the TTY_SERIAL
backend — this is a per-target choice configured in `target_late_init()`.
The UART hardware is still initialized either way (for non-semihosting
fallback).

---

## 4. Test Script Changes

When `PPAP_SEMIHOST` is enabled, `run.sh` adds `-semihosting` to the QEMU
command line (alongside the existing `-serial mon:stdio`):

```sh
# Default (UART only):
qemu-system-arm -M mps2-an500 -serial mon:stdio -nographic -kernel $ELF

# With semihosting (UART still present):
qemu-system-arm -M mps2-an500 -serial mon:stdio -semihosting -nographic -kernel $ELF
```

Both UART and semihosting output appear on QEMU's stdout, so the test output
parsing ("ALL TESTS PASSED" check) works unchanged.

---

## 5. Files to Create and Modify

### New files

```
src/drivers/arch/arm_m/semihost.c   — semihost_call(), uart.h API implementation
src/drivers/arch/arm_m/semihost.h   — public declarations (if needed)
```

### Files to modify

| File | Change |
|------|--------|
| `cmake/arm_m.cmake` | Add `PPAP_SEMIHOST` option; compile `semihost.c` alongside UART driver |
| `scripts/run.sh` | Detect semihosting build; add `-semihosting` to QEMU args |

### Files that need NO changes

| File | Reason |
|------|--------|
| `src/kernel/fd/tty.c` | Backend-agnostic |
| `src/kernel/klog.c` | Calls `uart_putc()` — semihost.c provides this |
| `src/kernel/fs/devfs.c` | Calls `uart_putc()` / `uart_getc()` — same |
| All target files | `PPAP_SEMIHOST` is handled at the cmake/driver level |
| All user-space code | Unchanged |
| All test code | Unchanged |

---

## 6. Implementation Steps

### Step 1: Implement semihost driver

Create `src/drivers/arch/arm_m/semihost.c` implementing the `uart.h` API:

- `uart_init()` — no-op (no hardware to initialize)
- `uart_putc()` — `bkpt 0xAB` with SYS_WRITEC
- `uart_getc()` — `bkpt 0xAB` with SYS_READC (QEMU: non-blocking)
- `uart_rx_avail()` — return 0 (no interrupt-driven RX)
- `uart_flush()` — no-op

### Step 2: Add build flag

In `cmake/arm_m.cmake`:

```cmake
option(PPAP_SEMIHOST "Enable ARM semihosting as additional serial channel" OFF)

# UART driver is always compiled (primary serial backend)
if(PPAP_SEMIHOST)
    target_sources(... semihost.c)
    target_compile_definitions(... PPAP_SEMIHOST=1)
endif()
```

### Step 3: Update run.sh

Add `-semihosting` when the build has semihosting enabled:

```sh
QEMU_ARGS=(-M mps2-an500 -serial mon:stdio -nographic)

# Detect semihosting build and add flag (UART is still present)
if [[ -f "$BUILD_DIR/semihost_enabled" ]] || \
   nm "$ELF" 2>/dev/null | grep -q semihost_call; then
    QEMU_ARGS+=(-semihosting)
fi
```

### Step 4: Test

```sh
cmake -B build/arm_m -DPPAP_SEMIHOST=ON ...
./scripts/run.sh --test qemu_arm
# Expect: "ALL TESTS PASSED" via semihosting output
```

---

## 7. Risks and Mitigations

### 7.1 Performance

Semihosting is slow — each `bkpt` traps to the debugger and back. For bulk
output this is ~100x slower than a real UART ring buffer with ISR drainage.

**Mitigation**: Use `SYS_WRITE` (0x05) for multi-byte output instead of
per-character `SYS_WRITEC`. The TTY layer currently calls `putc` per
character, but a future optimization could batch writes.

For testing purposes, performance is not a concern.

### 7.2 HardFault Without Debugger

If `bkpt 0xAB` executes without a semihosting-aware debugger attached, it
triggers a HardFault (or debug monitor exception).

**Mitigation**: `PPAP_SEMIHOST` is opt-in (off by default). Users must
explicitly enable it and run under QEMU with `-semihosting` or OpenOCD with
`arm semihosting enable`.

### 7.3 Input on OpenOCD

`SYS_READC` blocks the CPU on OpenOCD, which would stall the kernel.

**Mitigation**: For the initial implementation, input is polling-based and
best-effort. Interactive input via semihosting on real hardware is a stretch
goal. Output-only (klog + test results) covers the primary use case.

### 7.4 Interaction with FPB (Flash Patch and Breakpoint)

`bkpt 0xAB` is a software breakpoint, not an FPB hardware breakpoint. It
does not consume any FPB comparator slots.

**No conflict** with PPAP's ptrace/hardware breakpoint infrastructure.

---

## 8. Future Extensions

- **m68k semihosting** — QEMU m68k supports semihosting via `trap #0` with
  a different calling convention. Could add `PPAP_SEMIHOST` for m68k targets.
- **Bulk write optimization** — Batch TTY output into `SYS_WRITE` calls
  instead of per-character `SYS_WRITEC`.
- **SYS_EXIT** (0x18) — Use semihosting exit to report test pass/fail
  status to QEMU (QEMU exits with the given code). Would make CI scripts
  simpler (check exit code instead of parsing stdout).
- **RISC-V semihosting** — For future Hazard3 (RP2350 RISC-V) support,
  the 3-instruction `ebreak` sequence follows the same protocol.
