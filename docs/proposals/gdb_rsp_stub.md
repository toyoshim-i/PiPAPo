# GDB RSP Stub for PPAP Kernel Debugging

> **Note**: File paths in this document may be outdated after the source tree
> reorganization.  See [Source Tree Structure](../getting_started/source_tree.md)
> for the current layout.

## Motivation

Source-level debugging of the PPAP kernel and userland apps currently requires
either JTAG/SWD hardware (OpenOCD) or QEMU's built-in GDB server.  A GDB
Remote Serial Protocol (RSP) stub embedded in the kernel would enable
source-level debugging over a plain UART serial connection on any target —
no debug probe needed.

No custom VSCode extension is required.  The existing C/C++ or cortex-debug
extensions already speak GDB's MI protocol; only a `launch.json` configuration
is needed on the host side.

## Architecture Overview

```
+---------------+     UART (serial)     +----------------+
|   Host GDB    |<--------------------->|   GDB Stub     |
|   (or VSCode  |   GDB RSP packets    |   in kernel    |
|    via MI)    |   $packet#checksum   |                |
+---------------+                       +--------+-------+
                                                 |
                                        +--------v-------+
                                        |   Arch layer   |
                                        |  (per-arch     |
                                        |   reg r/w,     |
                                        |   single-step, |
                                        |   breakpoints) |
                                        +--------+-------+
                                                 |
                                        +--------v-------+
                                        |  PPAP Kernel   |
                                        |  (ptrace infra,|
                                        |   trap handlers|
                                        |   proc_table)  |
                                        +----------------+
```

## GDB RSP Packet Summary

All packets use the framing `$<data>#<2-hex-digit-checksum>`.
The receiver replies `+` (ACK) or `-` (NACK).

### Core packets (Phase 1)

| Packet          | Function               | Notes                              |
|-----------------|------------------------|------------------------------------|
| `?`             | Stop reason            | Returns `S05` (SIGTRAP)            |
| `g`             | Read all registers     | Delegates to arch layer            |
| `G XX...`       | Write all registers    | Delegates to arch layer            |
| `m addr,len`    | Read memory            | Direct memory read (kernel space)  |
| `M addr,len:XX` | Write memory           | Direct memory write                |
| `c [addr]`      | Continue               | Resume execution                   |
| `s [addr]`      | Single step            | Arch-specific                      |
| `Z0,addr,len`   | Set SW breakpoint      | Insert trap instruction            |
| `z0,addr,len`   | Remove SW breakpoint   | Restore original instruction       |

### Extended packets (Phase 2+)

| Packet              | Function             | Notes                            |
|----------------------|----------------------|----------------------------------|
| `Z1,addr,len`       | Set HW breakpoint    | Arch-specific (FPB/debug regs)   |
| `z1,addr,len`       | Remove HW breakpoint | Arch-specific                    |
| `Hg n`              | Set thread/process   | Select which process to inspect  |
| `qfThreadInfo`      | List threads         | Enumerate proc_table             |
| `qsThreadInfo`      | Continued listing    | Returns `l` when done            |
| `qC`                | Current thread       | Return current pid               |
| `T tid`             | Thread alive?        | Check proc_table state           |
| `vCont;c` / `vCont;s` | Continue/step      | Per-thread control               |
| `qXfer:features`    | Target description   | Arch-specific XML                |

## Architecture Layer Interface

Each architecture implements:

```c
typedef struct gdb_arch_ops {
    int reg_count;                // number of registers
    int reg_size;                 // total bytes for all registers

    void (*read_regs)(const pcb_t *proc, uint32_t *regs);
    void (*write_regs)(pcb_t *proc, const uint32_t *regs);

    bool (*set_single_step)(pcb_t *proc);
    void (*clear_single_step)(pcb_t *proc);

    const uint8_t *sw_break_insn; // breakpoint instruction bytes
    int sw_break_size;            // 2 for Thumb/m68k, 4 for RV32, 3 for Xtensa

    int hw_break_max;             // 0 if unsupported
    bool (*set_hw_break)(uintptr_t addr);
    bool (*clear_hw_break)(uintptr_t addr);

    const char *target_xml;       // for qXfer:features (may be NULL)
} gdb_arch_ops_t;
```

### Per-arch specifics

| Arch           | Registers (GDB order)              | SW break insn        | Single-step mechanism            | HW breaks                    |
|----------------|-------------------------------------|----------------------|----------------------------------|------------------------------|
| ARM Cortex-M   | r0-r12, sp, lr, pc, xpsr (17 regs) | `BKPT #0` (0xBE00)  | FPB comparator (v7-M/v8-M) or SW emulation | FPB: 4-6 on v7-M/v8-M, 0 on v6-M |
| m68k           | d0-d7, a0-a7, sr, pc (18 regs)     | `ILLEGAL` (0x4AFC)   | SR trace bit (bit 15) — native  | None                         |
| RISC-V RV32    | x0-x31, pc (33 regs)               | `EBREAK` (0x00100073) | SW breakpoint at next instruction | Trigger module (if present)  |
| Xtensa LX7     | a0-a15, pc, ps, sar... (variable)  | `BREAK 1,0` (3 bytes) | ICOUNT + ICOUNTLEVEL            | 2 IBREAK registers           |

## Entry Points

The stub enters `gdb_handle_exception()` from:

1. **Breakpoint hit** — trap/fault handler detects breakpoint instruction
2. **Single-step complete** — trace exception (m68k SR.T) or step-breakpoint
3. **Manual break** — Ctrl+C from GDB sends 0x03 byte on UART; RX ISR sets a
   flag, checked at next safe point (return from any ISR)
4. **Explicit call** — `gdb_breakpoint()` function inserted in kernel code
5. **Fault** — HardFault/BusError can optionally enter the stub instead of
   crashing

```c
// Called from trap handlers when a debug event occurs.
// Enters polled command loop until GDB sends 'c' or 's'.
void gdb_handle_exception(int signum, void *frame);
```

While in the command loop, the kernel is halted.  SysTick/timer interrupts
are masked (inherently, since we are in a trap handler context with interrupts
disabled or at elevated priority).

## UART I/O — Polled Mode

During a debug stop the interrupt-driven ring buffer cannot be used.  The stub
uses direct polled I/O:

```c
static int  gdb_uart_getc(void);  // spin-wait on RX FIFO not empty
static void gdb_uart_putc(int c); // spin-wait on TX FIFO not full
```

Each target's UART hardware registers are already known from the existing
driver implementations.  The polled functions simply bypass the ISR path.

Console output while debugging is forwarded to GDB via the `O` packet
(`$Ohexdata#xx`), so `printf` continues to work when UART is in GDB mode.

## Kernel Integration

### Trap handler hook (ARM example)

```c
void HardFault_Handler(void) {
    uint32_t *frame = ...;
    uint16_t insn = *(uint16_t *)frame[6];  // instruction at PC

    if (insn == 0xBE00 && gdb_attached) {
        gdb_handle_exception(SIGTRAP, frame);
        return;  // resume
    }
    // ... existing crash handling ...
}
```

### Scheduler awareness

The stub runs with interrupts disabled in trap handler context, so no context
switches occur.  When inspecting other processes via `Hg n`, the stub reads
from `proc_table[n]` directly (registers are saved in the PCB when not
running).

### Existing ptrace infrastructure

PPAP already has comprehensive ptrace support (`src/common/ptrace.h`):

- `PTRACE_GETREGS` / `PTRACE_SETREGS`
- `PTRACE_PEEKDATA` / `PTRACE_POKEDATA`
- `PTRACE_SINGLESTEP` / `PTRACE_CONT`
- `PPAP_PTRACE_BP_SW` / `PPAP_PTRACE_BP_HW`
- `PROC_TRACED_STOP` state

The GDB stub's arch layer can share implementation with ptrace where possible.

## File Structure

```
src/kernel/debug/
  gdb_stub.c        # Protocol layer — arch-independent (~600 lines)
  gdb_stub.h        # Public API: gdb_init(), gdb_breakpoint(),
                     #             gdb_handle_exception()
  gdb_arch.h        # gdb_arch_ops_t interface definition

src/arch/arm_m/gdb_arch.c     # ARM: register map, FPB, BKPT
src/arch/m68k/gdb_arch.c      # m68k: register map, SR.T, ILLEGAL
src/arch/riscv/gdb_arch.c     # RISC-V: register map, EBREAK, SW step
src/arch/xtensa/gdb_arch.c    # Xtensa: register map, BREAK, ICOUNT
```

## VSCode Integration

No custom extension needed.  A `launch.json` configuration suffices.

### Serial debugging (real hardware)

Use `socat` to bridge serial to TCP:

```sh
socat TCP-LISTEN:1234,reuseaddr /dev/ttyACM0,b115200,raw
```

```json
{
    "name": "PPAP Kernel Debug (Serial)",
    "type": "cppdbg",
    "request": "launch",
    "program": "${workspaceFolder}/build/pico1/ppap",
    "MIMode": "gdb",
    "miDebuggerPath": "gdb-multiarch",
    "miDebuggerServerAddress": "localhost:1234",
    "setupCommands": [
        { "text": "set architecture arm" }
    ]
}
```

### QEMU debugging

QEMU already has a built-in GDB server (`-s -S` flags); the in-kernel stub
is not needed for this case but can coexist.

```json
{
    "name": "PPAP Kernel Debug (QEMU)",
    "type": "cppdbg",
    "request": "launch",
    "program": "${workspaceFolder}/build/qemu_arm/ppap",
    "MIMode": "gdb",
    "miDebuggerPath": "gdb-multiarch",
    "miDebuggerServerAddress": "localhost:1234",
    "setupCommands": [
        { "text": "set architecture arm" }
    ]
}
```

## Implementation Phases

| Phase   | Scope                                                                 | Est. LOC |
|---------|-----------------------------------------------------------------------|----------|
| Phase 1 | Minimal stub: ARM only, `?`/`g`/`m`/`c`/`Z0`/`z0`, polled UART, kernel-only | ~500     |
| Phase 2 | Add m68k + RISC-V, single-step, HW breakpoints, `qfThreadInfo`       | ~400     |
| Phase 3 | Userland debugging via existing ptrace infra, `Hg`, `vCont`           | ~300     |
| Phase 4 | Xtensa support, `qXfer:features` target XML, watchpoints             | ~200     |

## Design Decisions

### Shared vs dedicated UART

The stub shares UART0 with normal console output.  When GDB is attached,
`printf` output is tunneled through GDB `O` packets (hex-encoded console
output).  A compile-time `#define GDB_ENABLED` controls inclusion of the
stub.

### Entry mechanism on real hardware

A sequence of 3x 0x03 bytes (Ctrl+C) on UART triggers a break into the
debugger.  The UART RX ISR counts consecutive 0x03 bytes and sets a
`gdb_break_requested` flag.  The flag is checked on return from any ISR
(similar to signal delivery).

### Kernel vs userland

Phase 1 debugs the kernel in a single flat address space.  For userland
debugging (Phase 3), the stub uses the existing ptrace infrastructure to
read/write user process registers and memory.  On Cortex-M33 targets with
MPU, the stub temporarily adjusts MPU regions to access user memory.

### Interaction with existing ptrace

The GDB stub and ptrace serve different roles:

- **ptrace**: in-kernel API for one PPAP process to trace another
- **GDB stub**: external host debugger inspecting the kernel (and processes)
  via serial

They share the per-arch register read/write code but operate independently.
The GDB stub can debug processes that have no tracer attached.
