# PPAP Documentation Index

This index is the entry point for project documentation.

## Naming convention

- Directory and file names use `underbar_style`.
- Avoid duplicated prefixes inside a subdirectory.
  Example: `subsystems/human68k.md` (not `subsystems/subsystem_human68k.md`).

## Start here

- Project overview and quick commands: [`../README.md`](../README.md)
- Design spec: [`spec_v07.md`](spec_v07.md)
- Build/run/test workflow: [`getting_started/quick_start.md`](getting_started/quick_start.md)

## Sections

- Getting started
  - [`getting_started/quick_start.md`](getting_started/quick_start.md)
  - [`getting_started/build_and_run.md`](getting_started/build_and_run.md)
  - [`getting_started/testing.md`](getting_started/testing.md)
  - [`getting_started/debugging.md`](getting_started/debugging.md)
  - [`getting_started/porting.md`](getting_started/porting.md)
  - [`getting_started/coding_rules.md`](getting_started/coding_rules.md)

- User (end-user app and user-space references)
  - [`user/push.md`](user/push.md) — push (PiPAPo μShell)
  - [`user/calc.md`](user/calc.md) — calc (programmer's calculator)
  - [`user/userland_dev_guide.md`](user/userland_dev_guide.md) — Writing user-space programs
  - [`user/trace.md`](user/trace.md) — Trace and debug subsystem (/bin/trace, /bin/pdb)

- Kernel
  - [`kernel/overview.md`](kernel/overview.md)
  - [`kernel/modules.md`](kernel/modules.md) — Module system (mod_core, mod_vfs)
  - [`kernel/context_switch.md`](kernel/context_switch.md) — Per-architecture context switching
  - [`kernel/stack.md`](kernel/stack.md) — Per-architecture kernel stack mechanics
  - [`kernel/memory.md`](kernel/memory.md)
  - [`kernel/syscall.md`](kernel/syscall.md)
  - [`kernel/filesystems.md`](kernel/filesystems.md)
  - [`kernel/ufs.md`](kernel/ufs.md)
  - [`kernel/procfs.md`](kernel/procfs.md)

- Proposals
  - [`proposals/x68k_port.md`](proposals/x68k_port.md) — X68000 m68k port
  - [`proposals/pizero_port.md`](proposals/pizero_port.md) — Raspberry Pi Zero (future)
  - [`proposals/msdos_subsystem.md`](proposals/msdos_subsystem.md) — MS-DOS personality subsystem
  - [`proposals/i8086_ecpu.md`](proposals/i8086_ecpu.md) — i8086 eCPU emulator
  - [`proposals/gdb_rsp_stub.md`](proposals/gdb_rsp_stub.md) — GDB Remote Serial Protocol stub

  - [`proposals/m33_mpu_full_protection.md`](proposals/m33_mpu_full_protection.md) — Cortex-M33 MPU full protection

- Subsystems
  - [`subsystems/overview.md`](subsystems/overview.md)
  - [`subsystems/human68k.md`](subsystems/human68k.md)
  - [`subsystems/cpm.md`](subsystems/cpm.md)
  - [`subsystems/sos.md`](subsystems/sos.md) (S-OS SWORD)

- eCPU
  - [`ecpu/overview.md`](ecpu/overview.md)
  - [`ecpu/m68k.md`](ecpu/m68k.md)
  - [`ecpu/z80.md`](ecpu/z80.md)

- Targets
  - [`targets/arm_m.md`](targets/arm_m.md) — ARM Cortex-M (qemu_arm, pico1, pico1calc, pico2)
  - [`targets/rv32.md`](targets/rv32.md) — RISC-V RV32IMAC (qemu_rv32, pico2rv)
  - [`targets/m68k.md`](targets/m68k.md) — Motorola 68000 (qemu_m68k, x68k)
  - [`targets/ia16.md`](targets/ia16.md) — Intel 8086 (pcxt)
  - [`targets/xtensa.md`](targets/xtensa.md) — Xtensa LX7 (xtensa_cc / M5Stack CardComputer)
  - [`targets/host.md`](targets/host.md) — Native Linux/macOS (host build of push and pi)

- Notes
  - [`notes/zombie.md`](notes/zombie.md) — Process lifecycle: zombies, orphans, reaping
  - [`notes/x68k_bootstrap.md`](notes/x68k_bootstrap.md) — X68000 two-stage floppy boot
  - [`notes/x68k_tty_architecture.md`](notes/x68k_tty_architecture.md) — X68000 TTY/display architecture

- Reference
  - [`reference/picocalc.md`](reference/picocalc.md)
  - [`reference/picocalc_lcd.md`](reference/picocalc_lcd.md)

- Archive
  - [`archive/history/`](archive/history/) — Phase plans, development notes
  - [`archive/ports/`](archive/ports/) — Completed port proposals (pico2, pico2rv)
  - [`archive/subsystems/`](archive/subsystems/) — Completed subsystem proposals (CP/M, S-OS)
  - [`archive/kernel/`](archive/kernel/) — Completed kernel refactoring docs
