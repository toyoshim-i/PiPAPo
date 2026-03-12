# Debugging

## ARM hardware (OpenOCD + GDB)

```sh
# Terminal 1
openocd -f scripts/debug/openocd.cfg

# Terminal 2
gdb-multiarch -x scripts/debug/pico1calc.gdb build/pico1calc/ppap_pico1calc.elf

# Or attach to running firmware
gdb-multiarch -x scripts/debug/pico1calc-attach.gdb build/pico1calc/ppap_pico1calc.elf
```

## QEMU notes

- `./scripts/run.sh --gdb` starts the existing binary under debugger flow.
- For test-mode debugging, use `./scripts/run.sh --test <target>` and inspect
  the UART output for per-test PASS/FAIL summaries.

## Strace-style tracing (`trace`)

PPAP includes a user-space tracer (`/bin/trace`) that works like a lightweight
`strace` for PPAP syscalls and subsystem bridge calls.

```sh
# Trace PPAP syscalls only
trace --ppap /bin/hello

# Trace subsystem bridge calls only (Human68k/CP/M)
trace --subsys /subsys/human68k/hello2.x

# Trace both and include register snapshots
trace --both --regs /bin/hello
```

Notes:

- If no mode is specified, `trace` defaults to `--both`.
- `--subsys` output requires a subsystem process (for example Human68k or CP/M)
  and that subsystem support is enabled in the build.
- For trace architecture and event model details, see
  [`../kernel/trace.md`](../kernel/trace.md).
- For the next debugger implementation phase (single-step/breakpoints/caps),
  see [`../proposals/debugger_ptrace.md`](../proposals/debugger_ptrace.md).

## Userland debugger (`pdb`)

PPAP also includes a minimal interactive debugger (`/bin/pdb`) built on
`ptrace`.

```sh
# Launch target under debugger
pdb /bin/hello

# Typical commands inside pdb:
#   regs
#   caps
#   x 0x20000000 4
#   step
#   cont
#   break 0x0101
#   delete 0
#   quit
```
