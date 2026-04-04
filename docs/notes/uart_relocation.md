# UART / klog Relocation Plan: Core → VFS

## Motivation

`klog`, `uart`, and display drivers (`bios_con`, `fbcon`, `semihost`)
are currently in the core module, but their consumers are VFS (TTY,
devfs) and target init (logger registration).  Moving them to VFS
eliminates vfs→core include violations for these headers and aligns
ownership with the actual I/O path: user → TTY → uart.

## Steps

### 1. Move klog to VFS, drop klog()

- `kernel/core/klog.c` → `kernel/vfs/klog.c`
- `kernel/core/klog.h` → `kernel/vfs/klog.h`
- Remove `klog()` — all call sites use `klogf()` instead
- Add `klogf` to `mod_vfs.inc` / `mod_vfs.h`
- Remove `klog` / `klogf` from `mod_core.inc` / `mod_core.h`
- Core code calls `mod_vfs.klogf()` for all logging

`klog.c` uses `arch_preempt_disable/enable` and `spin_lock/unlock` —
both are inline (arch overlay + `spinlock.h`), so no new mod_core
entries are needed.

### 2. Move uart + display drivers to VFS side

| File | From | To |
|------|------|----|
| `uart.h` | `kernel/core/driver/` | `kernel/vfs/` |
| `uart_rpico.c/h` | `arch/arm_m/kernel/core/driver/` | `arch/arm_m/kernel/vfs/driver/` |
| `uart_rp2350.c/h` | `arch/riscv/kernel/core/driver/` | `arch/riscv/kernel/vfs/driver/` |
| `uart_esp32s3.c` | `arch/xtensa/kernel/core/driver/` | `arch/xtensa/kernel/vfs/driver/` |
| `semihost.c` | `arch/arm_m/kernel/core/driver/` | `arch/arm_m/kernel/vfs/driver/` |
| `uart_qemu.c` | `target/qemu_arm/kernel/core/driver/` | `target/qemu_arm/kernel/vfs/driver/` |
| `uart_qemu_m68k.c` | `target/qemu_m68k/kernel/core/driver/` | `target/qemu_m68k/kernel/vfs/driver/` |
| `uart_ns16550.c` | `target/qemu_rv32/kernel/core/driver/` | `target/qemu_rv32/kernel/vfs/driver/` |
| `uart_x68k.c` | `target/x68k/kernel/core/driver/` | `target/x68k/kernel/vfs/driver/` |
| `uart_com.c` | `target/pcxt/kernel/core/driver/` | `target/pcxt/kernel/vfs/driver/` |
| `bios_con.c/h` | `target/pcxt/kernel/core/driver/` | `target/pcxt/kernel/vfs/driver/` |
| `pcxt_logger.c/h` | `target/pcxt/kernel/core/driver/` | `target/pcxt/kernel/vfs/driver/` |
| `fbcon.c/h` | `kernel/core/driver/` | `kernel/vfs/` |

### 3. Move logger init from core to VFS

Each target's `target_early_init()` currently calls:
```c
uart_init();
klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
```

This moves into the VFS init path.  Options:
- `mod_vfs.init()` calls a target-provided hook for logger setup
- Target provides a VFS-side init function registered during build

`klog_set_logger()` stays VFS-internal (not in mod_vfs).

### 4. Update ia16/pcxt stubs

- Remove `klog` (idx 2) and `klogf` (idx 3) from `mod_core.inc`
- Renumber remaining mod_core entries (idx 2+ shift down by 2)
- Add `klogf` to `mod_vfs.inc`
- Update `core_entries.S`, `core_stubs.S` (auto-generated from .inc)
- Update `vfs_entries.S`, `vfs_stubs.S` (auto-generated from .inc)
- Update `target_pcxt.c` core→VFS far pointer patching (count changes)

### 5. Update build files

- `cmake/kernel.cmake`: move klog.c, fbcon.c from core to VFS sources
- All `target/*/CMakeLists.txt`: move uart impl sources to VFS target
- `arch/*/CMakeLists.txt` or `cmake/*.cmake`: move arch uart to VFS

## Risks

- **Boot ordering**: `mod_vfs.init()` must run before any `klogf()`
  call.  Stage1/stage2 boot loaders have their own output — kernel
  logging starts after VFS init.
- **ia16 far-call cost**: every `klogf` from core goes through a
  far call.  Acceptable — klog is diagnostic, not hot-path.
- **pcxt dual-compile**: uart_com.c is currently compiled into both
  core and VFS modules.  After the move, it's VFS-only.  Core's
  direct UART access (if any) must go through mod_vfs.

## Verification

- `./scripts/build.sh qemu_arm`
- `./scripts/build.sh qemu_m68k`
- `./scripts/build.sh qemu_rv32`
- `./scripts/build.sh pcxt`
- `./scripts/run.sh --test qemu_arm`
- `./scripts/run.sh --test qemu_m68k`
- Boot test on pcxt (QEMU) — verify klog output appears
