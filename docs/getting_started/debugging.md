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

`disas` currently supports:

- ARM real-surface tracees (Thumb subset decoder with raw halfword fallback)
- Z80 eCPU tracees
- m68k tracees (native or eCPU surface)

```sh
# Launch target under debugger
pdb /bin/hello

# Attach to an already running process
pdb --attach 42

# Scripted mode (for automation/tests)
pdb -c "caps" -c "regs" -c "disas 0x0100 3" /tmp/prog.com

# Quiet scripted mode (suppress prompt/command echo)
pdb -q -c "show sp" -c "cont" /tmp/prog.com

# Batch scripted mode (suppress prompt/echo and automatic stop/target lines)
pdb --batch -c "show event" -c "show caps" -c "cont" /tmp/prog.com

# Script file mode
pdb -f /tmp/pdb.script /tmp/prog.com

# Typical commands inside pdb:
#   regs
#   reg pc
#   caps
#   event
#   show abi
#   show event
#   show caps
#   show regset
#   show pc
#   show sp
#   show surface
#   where
#   x/4x 0x20000000
#   x/8h 0x20000000
#   x/16b 0x20000000
#   x 0x20000000 4
#   disas 0x0100 8
#   event          # includes decoded debug-stop reason (step/sw-bp/hw-bp)
#   set reg pc 0x0100
#   set mem 0x0100 0x00000000
#   set mem 0x0101 0x34 1
#   set mem 0x0102 0x5678 2
#   set mem 0x0100 0x89abcdef w
#   step / s
#   next / n
#   run / cont / continue / c
#   break / b 0x0101
#   disable 0
#   enable 0
#   delete / d 0
#   info break / b
#   detach
#   quit / q
```

`-f` script files are plain text:

- One command per line.
- Leading and trailing spaces/tabs are ignored.
- Blank lines are ignored.
- A line whose first non-space character is `#` is treated as a comment.
- Both LF and CRLF line endings are accepted.
- If a script resolves to zero commands, `pdb` exits with an error.

Scripted-mode notes:

- `-q` suppresses prompt and command-echo output for cleaner automation logs.
- `--batch` also suppresses automatic initial stop/target and child-exit lines.
- `caps`/`show caps` prints both hex capability bits and decoded names.
- `break` support is mapping-dependent; on unsupported targets `pdb` prints
  `break not supported on this target/mapping`.
- `surface ecpu` on native-only targets fails with `SETSURFACE failed rc=...`
  and includes a support hint.
- `set mem ... h` requires an even address and must stay within one 32-bit word.
- `set mem` rejects values that do not fit the selected width (`b`/`h`).
- `set mem` size accepts `b|h|w` or numeric aliases `1|2|4`.
- Whitespace-only `-c` entries are ignored.
- Overlong `-c` commands and overlong script lines are rejected with an error.
