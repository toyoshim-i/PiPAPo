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
  - [`getting_started/build_system.md`](getting_started/build_system.md)
  - [`getting_started/push.md`](getting_started/push.md)
  - [`getting_started/userland_dev_guide.md`](getting_started/userland_dev_guide.md)
  - [`getting_started/porting.md`](getting_started/porting.md)
  - [`getting_started/commit_description.md`](getting_started/commit_description.md)

- Kernel
  - [`kernel/overview.md`](kernel/overview.md)
  - [`kernel/memory.md`](kernel/memory.md)
  - [`kernel/syscall.md`](kernel/syscall.md)
  - [`kernel/procfs.md`](kernel/procfs.md)
  - [`kernel/trace.md`](kernel/trace.md)
  - [`kernel/filesystems.md`](kernel/filesystems.md)

- Proposals
  - [`proposals/cardcomputer_port.md`](proposals/cardcomputer_port.md) — CardComputer device support (display, keyboard, SD)
  - [`proposals/debugger_ptrace.md`](proposals/debugger_ptrace.md)

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
  - [`targets/68000.md`](targets/68000.md) — Motorola 68000 (qemu_m68k, x68k)
  - [`targets/xtensa.md`](targets/xtensa.md) — Xtensa LX7 (xtensa_cc / M5Stack CardComputer)
  - [`targets/pizero.md`](targets/pizero.md) — Pi Zero (future)

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
