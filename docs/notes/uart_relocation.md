# UART / klog Relocation: Core → VFS (COMPLETE)

## Summary

Moved `klog`, `uart`, and display drivers from the core module to VFS.
This eliminates core→VFS include violations for these headers and aligns
ownership with the actual I/O path: user → TTY → uart.

Completed in two commits:
- `60f9d6b` — Step 1: move klog, drop klog(), add klogf/klog_set_logger to mod_vfs
- `d86a008` — Steps 2+3: move uart/display drivers, add klog_logger_init hook

## What changed

### klog (step 1)
- `kernel/core/klog.c/h` → `kernel/vfs/klog.c/h`
- Removed `klog()` — all sites use `klogf()`
- `klogf` + `klog_set_logger` added to mod_vfs; removed from mod_core
- Core calls `mod_vfs.klogf()`; VFS calls `klogf()` directly

### uart + display drivers (step 2)
- `uart.h`, `fbcon.c/h` → `kernel/vfs/`
- All uart implementations → `kernel/vfs/driver/` in their arch/target
- `semihost.c`, `bios_con.c/h`, `pcxt_logger.c/h` → `kernel/vfs/driver/`

### Logger init (step 3)
- Weak `klog_logger_init()` in klog.c, called from `vfs_init()`
- Each target overrides in a VFS-side `*_logger.c` file
- `target_early_init()` no longer calls `uart_init()`/`klog_set_logger()`
- pcxt logger is fully VFS-side (no far call needed on i16)

### ia16/pcxt stubs (step 4) — done in step 1
- mod_core: 20→18 entries (klog/klogf removed)
- mod_vfs: 37→39 entries (klogf/klog_set_logger added)
- PATCH_CORE table renumbered in target_pcxt.c

### Build files (step 5) — done in steps 1–3
- cmake/kernel.cmake, all target CMakeLists.txt updated
- xtensa_cc ESP-IDF component CMakeLists.txt updated

## Remaining work

- **vfs.h/ufs.h in core** (qemu_rv32, x68k): `target_mount_rootfs()`
  calls `vfs_mount()` directly.  Not uart-related -- separate concern
  (VFS mount API exposure to core targets).

## Verification

All 9 targets build.  qemu_arm 24/24 user tests pass.
pcxt uart_com.c/bios_con.c/pcxt_logger.c are VFS-only (no dual-compile).
