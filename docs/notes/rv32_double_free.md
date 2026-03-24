# RISC-V Double-Free / Crash Investigation

Status: **in progress** (2026-03-24)

## Symptom

Busybox commands (`ls`, `ps`, `df`) crash on both pico2rv hardware and
QEMU rv32.  On hardware, it manifests as a double-free; on QEMU, it's a
load access fault:

```
PiPaPo:/# ls /
TRAP: exception cause=0x00000005 mepc=0x80034bc2 mtval=0x00000008 pid=3
```

- **cause=5**: load access fault
- **mtval=0x8**: faulting address (near-NULL pointer dereference)
- **pid=3**: the busybox `ls` child process

Simple applets like `echo hello` work fine.

## Root Cause Analysis

### 1. Crash is in kernel `strlen`, during `execve`

The faulting `mepc` maps to the kernel's custom `strlen()` function
(in `src/target/qemu_rv32/string.c`), NOT busybox's musl `strlen`.

Debug syscall tracing confirmed the crash happens during **SYS_EXECVE**
(syscall 0x0003), not during normal ls operation.

### 2. `strlen(argv[i])` reads address 0x8

In `elf_loader.c` line 609:
```c
uint32_t len = (uint32_t)strlen(argv[i]) + 1;
```

The shell (`hush`) does `vfork + execve("/bin/ls", argv, NULL)`.
The `argv` array lives on the shell's user stack.  During `execve`,
the kernel reads `argv[i]` to copy argument strings to the new
process stack.  One of the `argv[i]` pointers is `0x8`, causing the
load fault.

### 3. Why is argv[i] = 0x8?

This is the key unsolved question.  Hypotheses:

- **GOT relocation incomplete**: the shell's string pointers weren't
  fully relocated when busybox was loaded.  The GOT patching in
  `elf_loader.c` only handles GOT entries and `R_RISCV_32` relocations,
  but `auipc`-based references (used for local statics) are
  PC-relative and should work without patching.

- **Page corruption during allocation**: `page_alloc_contiguous(49)`
  for busybox might overlap with or corrupt pages used by the shell.
  The shell uses 6 pages (loaded at 0x80809000); busybox allocates
  49 pages starting at 0x8080f000.  These don't overlap, but the
  page allocator's contiguous scan might have side effects.

- **User page leak in exec**: when `elf_loader.c` overwrites
  `p->user_pages[]` at line 421, the old pages are never freed.
  This is a leak but shouldn't cause the crash directly.

- **vfork argv pointer stale**: the parent's stack (where argv lives)
  should be valid during vfork since the parent is suspended.  But if
  the child's exec path somehow triggers a page free or reallocation
  that touches the parent's stack region, the pointers could become
  invalid.

### 4. The "double-free" on pico2rv

On hardware, the same root cause manifests differently because the
memory layout is tighter (fewer pages).  The corrupted argv pointer
may land in a previously freed page, causing the MM double-free
detection to fire.

## Verified Facts

- `echo hello` works: simple busybox applets that don't allocate
  much memory succeed
- `ls /` crashes: larger applets that read directories fail
- The crash happens during `execve`, not during `ls` execution
- The faulting address is always `0x8` (consistent NULL+offset)
- Busybox loads at 0x8080f000 (49 pages), shell at 0x80809000 (6 pages)

## Docker Build Fixes (committed)

While investigating, we fixed several issues blocking Docker-based
builds for qemu_rv32:

1. **Toolchain path**: `qemu_rv32/CMakeLists.txt` now respects
   `PICO_TOOLCHAIN_PATH` env var
2. **Hard-float libgcc fallback**: `user.cmake` detects hard-float
   Linux toolchain libgcc and falls back to bare-metal toolchain
3. **Rogue K&R fix**: `-std=gnu11` for GCC 15 C23 `()` = `(void)`
4. **Rogue PIE/strip**: conditional `-pie` vs `--emit-relocs`, plus
   `--strip-unneeded` to preserve relocation sections
5. **Busybox oldconfig**: `yes ""` for non-interactive config
6. **r68k test skip**: skip m68k cross-compilation when compiler
   unavailable

## Next Steps

1. **Disassemble the shell's argv construction**: find exactly where
   the shell builds the argv array for `execve`, and verify all
   pointer values are correctly relocated

2. **Add argv validation in execve**: before `strlen(argv[i])`, check
   that each pointer is within the page pool range.  Log and return
   `-EFAULT` if not.  This won't fix the root cause but will prevent
   the hard crash and give better diagnostics.

3. **Check page_alloc_contiguous for side effects**: verify that the
   49-page allocation for busybox doesn't corrupt the shell's 6 pages

4. **Test with simpler busybox applet**: try `cat /etc/hostname`
   (doesn't need directory reading) to narrow down whether the crash
   is specific to `ls` or any non-trivial execve
