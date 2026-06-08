# X68000 TTY Architecture

## Overview

The X68000 port provides dual-TTY console support:

- **tty1** (TTY_DISPLAY) — TVRAM text screen + keyboard, the primary console
- **ttyS0** (TTY_SERIAL) — RS-232C serial port via Z8530 SCC

Init spawns `/bin/sh` on tty1 (auto-login, no prompt) and
`/bin/getty ttyS0` on ttyS0 (with login prompt). See
`src/target/x68k/romfs/etc/inittab`.

## Layering

```
 User process (sh, getty)
        │
  read()/write() syscall
        │
  tty_read_canon / tty_write   ← line discipline, echo, signals
        │
  tty_backend_t  (.putc / .getc / .rx_avail)
        │
  ┌─────┴─────────────────────┐
  │ TTY_DISPLAY (tty1)        │ TTY_SERIAL (ttyS0)
  │ uart_putc  (IOCS _B_PUTC) │ uart_serial_putc  (IOCS _OUT232C)
  │ uart_getc  (IOCS _B_GETC) │ uart_serial_getc  (IOCS _INP232C)
  │ uart_rx_avail (_B_KEYSNS) │ uart_serial_rx_avail (_ISNS232C)
  └───────────────────────────┘
```

The kernel TTY layer (`src/kernel/vfs/tty.c`) is target-agnostic.
`target_late_init()` plugs in the X68000-specific backends via
`tty_set_backend()`.

## IOCS (IPL ROM) Dependency

All console I/O goes through X68000 IOCS calls (`TRAP #15`), which are
the IPL ROM's standard API for hardware access.  This has implications:

1. **Non-reentrancy**: IOCS functions share work areas in low RAM
   ($0400–$0FFF).  All IOCS calls are wrapped with `ipl7_save()`/
   `ipl7_restore()` to prevent preemption by the Timer-C ISR.

2. **Vector preservation**: Stage2 copies the kernel's `.vectors` to
   address 0, but must save and restore the IPL ROM's interrupt handlers:
   - TRAP #15 (vector 47): IOCS dispatch
   - Autovectors 24–31: VSYNC, SCC, OPM, etc.
   - MFP vectors 64–79: keyboard USART, timers, serial

   Without these, IOCS calls crash because the IPL ROM depends on its
   own interrupt handlers for VSYNC sync, keyboard input, etc.

3. **VT100 translation**: IOCS `_B_PUTC` has its own escape sequence
   parser (incompatible with VT100).  Sending raw VT100 CSI sequences
   causes address errors.  `uart_x68k.c` intercepts all ESC sequences,
   parses them as VT100, and converts supported sequences to equivalent
   IOCS calls (cursor positioning, screen clear, etc.).

## Input Polling

IOCS functions cannot be called from ISR or idle-loop context (they hang
or crash due to reentrancy and internal blocking).  Input availability is
polled using direct hardware register reads:

| TTY       | Poll function             | What it reads                        |
|-----------|---------------------------|--------------------------------------|
| tty1      | `uart_rx_avail_hw()`      | MFP USART RSR bit 7 at $E8802B      |
| ttyS0     | `uart_serial_rx_avail_hw()` | SCC ch.B RR0 bit 0 at $E98001     |

The timer ISR sets `input_poll_due` every 20 ms.  The idle loop calls
`sched_display_poll()`, which runs the poll functions and calls
`tty_rx_notify()` to wake blocked readers.

Actual character reads (when a process calls `read()`) use the IOCS
wrappers (`_B_KEYSNS`/`_B_GETC` for keyboard, `_ISNS232C`/`_INP232C`
for serial), which are safe in process context with IPL guarding.

## Preemptive Scheduling (Timer-C)

MFP Timer-C (vector 69) drives the 100 Hz scheduler tick.
`timer_init()` reconfigures Timer-C and overwrites vector 69 with
`m68k_timer_isr` (from `switch.S`), replacing the IPL ROM's Timer-C
handler.  This means IPL ROM Timer-C functionality (cursor blink, key
repeat timing) is lost.

## Known Limitation: No Input

**Status**: Shell prompt appears on tty1, getty message appears on
ttyS0, but keyboard and serial input do not work yet.

Possible causes under investigation:

1. **Keyboard**: The IPL ROM's MFP USART RX handler (vector 76,
   preserved by stage2) should buffer keystrokes at $081C.  The idle
   poll checks MFP USART RSR directly.  It's unclear whether the poll
   correctly detects keystrokes, or whether `_B_KEYSNS`/`_B_GETC` work
   after Timer-C has been taken over.

2. **Serial**: SCC channel B RR0 is polled for Rx availability.  IOCS
   `_ISNS232C`/`_INP232C` are used for actual reads.  The SCC interrupt
   handler (autovector 29, level 5) is preserved by stage2.  `_INP232C` is
   function `0x32` and `_ISNS232C` is function `0x33` per the local XEiJ IOCS
   definitions.

3. **Timer-C takeover**: Replacing the IPL ROM's Timer-C handler may
   break internal IOCS timing that keyboard or serial subsystems depend
   on.  Chaining to the old handler was attempted but caused regressions
   (the fake-exception-frame approach corrupted the switch.S context
   switch path).  Using IOCS `_B_INTVCS` to hook the vector also caused
   hangs — the IOCS call itself appears to have side effects.

## File Map

| File | Role |
|------|------|
| `src/target/x68k/kernel/vfs/driver/uart_x68k.c` | IOCS wrappers, VT100 converter, serial I/O |
| `src/target/x68k/kernel/core/driver/timer_x68k.c` | MFP Timer-C configuration |
| `src/target/x68k/kernel/vfs/driver/x68k_logger.c` | TTY backend wiring |
| `src/target/x68k/boot/stage2.c` | Vector preservation during kernel load |
| `src/target/x68k/romfs/etc/inittab` | Init config: sh on tty1, getty on ttyS0 |
| `src/kernel/vfs/tty.c` | Target-agnostic TTY line discipline |
| `src/arch/m68k/kernel/core/switch.S` | Timer ISR, context switch |
