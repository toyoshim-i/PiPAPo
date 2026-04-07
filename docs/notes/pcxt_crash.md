# PC/XT Post-Prompt Crash Investigation

## Symptom

After commit `3c34522` ("kernel: unify per-arch switch_pending into shared
arch_yield default"), `--clean` builds of `pcxt` boot to the shell prompt and
then crash.  Incremental rebuilds (no `--clean`) appear fine because some
stale `.o` files mask the underlying bug.

The crash is reported by `int_debug_handler` (target_pcxt.c) printing a
`FLAGS:CS` pair to COM1.  Observed value: `0602:0000`.

This pattern of "only on `--clean`, hidden by stale objects on incremental"
has been seen across several commits, suggesting the root cause is a file
with broken build-system dependency tracking that masks the real problem
until everything is rebuilt fresh.

## What we know from a QEMU `-d in_asm,cpu` trace

1. **Init starts correctly.** A breakpoint at linear `0x21000`
   (`CS=0x2100, IP=0`) hits with `SS:SP=0x2100:0x0fee` — exactly the
   layout `elf16_load_from_headers()` builds.  So the loader's initial
   `hw_frame[1] = proc_seg` is fine.
2. **Init runs through `int $0x30` with `AX=0x0003`** — that is
   `SYS_EXECVE`, so init is `execve`'ing its first child (`/bin/sh` /
   `push`).  The trap shows the user crashed *after* the shell printed
   its prompt, so this `execve` returned successfully (or its sibling
   `waitpid` was issued).
3. **The faulting `iret` is at offset `0x06a2` in core CS** — that is
   `i16_timer_isr`'s tail (`.Lno_switch`), not `i16_syscall_isr`.  So
   the moment of corruption is when the *timer ISR* tries to resume the
   user process.
4. **The user-stack IRET frame at `SS:SP = 0x2100:0x4120` reads
   `IP=0x010b, CS=0x0000, FLAGS=0x0602`.**  The `iret` pops `CS=0` and
   the CPU starts executing IVT bytes at linear `0x010b`, eventually
   hitting `INT 6` (invalid opcode) → `int_debug_handler`.

The user-stack neighbourhood right above the corrupt IRET frame is
24 words of `0x0096` — too uniform to be random GP-reg state, more
like a buffer that was filled with a constant before the trap.

## Hypotheses ranked

1. **vfork/execve stack hand-off + per-process kernel stack switch**:
   when init `execve`'s the shell on i16, the child shares the parent's
   user stack (vfork semantics), the loader rewrites the shared user
   stack with the new HW/SW frame, and `exec_pending` in trap.S
   rebuilds `[user_SP, user_SS]` on the kernel stack.  The window where
   the shared user stack is partially-rebuilt is fragile, and a timer
   tick that fires here can save/restore the wrong frame.  The fact
   that the iret in question is the timer ISR's (not the syscall ISR's)
   matches: timer fired *after* execve completed, while the new process
   was running, but the saved frame on the user stack came from a
   stale layout.
2. **Non-reentrant `vfs_*_entry` / `core_*_entry` static save area**:
   `vfs_entries.S` saves the far-return CS:IP into module-static
   `vfs_saved_cs/ip`, and the entry stub re-enables interrupts (`sti`)
   before calling the real C function.  If a context switch happens
   in that window and the *other* process makes its own far call, it
   stomps the static slot.  When the original process resumes and the
   stub `lret`s, it returns to the wrong CS:IP.  Same pattern in
   `core_entries.S`.  This is a real reentrancy bug regardless.
3. **`mm_page_write` / `mm_page_read` DS-as-scratch**: commit
   `af405b5` ("kernel: add argc/argv support for i16 ELF16 loader")
   already fixed one instance of `rep movs` not setting DS=SS first
   on i16.  There may be similar latent instances in other inline asm
   that touch user pages — those would be hidden until something
   triggers the bad register combination.

## Build-system dependency holes that mask bugs

This is the explanation for "only on `--clean`":

### `.S` files in core/VFS stub directories don't track `#include`d headers

- [src/target/pcxt/kernel/common/stubs/vfs_stubs.S](src/target/pcxt/kernel/common/stubs/vfs_stubs.S),
  `vfs_entries.S`, `vfs_header.S`, `core_stubs.S`, `core_entries.S`
  all `#include "kernel/common/mod/mod_vfs.inc"` /
  `mod_core.inc` (preprocessor `#include`, not `.include`).
- These files are listed in the kernel target's `add_executable()` source
  list, and `build/pcxt/CMakeFiles/ppap_pcxt.dir/.../*.S.obj` files exist —
  but **no `.S.d` (depfile) is generated** alongside them.  Verify with
  `find build/pcxt -name '*.S.d'` (returns nothing).
- Result: editing `mod_*.inc` (e.g. commit `42a84ec` removing
  `blkdev_read`/`blkdev_write` and shifting every later index down by
  two) does not trigger the `.S.obj` to rebuild.  The stale stub uses
  the OLD function indices into `core_fptrs[]` / `vfs_fptrs[]`,
  while the matching C-side `mod_*` struct is initialized with NEW
  indices.  Cross-module far calls jump to whatever the wrong slot
  happens to point at.

### `src/arch/i16/user/syscall.S` uses hardcoded literal syscall numbers

- Each wrapper has `movw $0x0103, %ax; int $0x30` etc. with the
  syscall number baked in as an integer literal.
- No `#include "common/syscall_nr.h"`, so renumbering syscalls in
  `syscall_nr.h` silently desynchronises user-space without any
  build-system signal at all.  Currently the literals match
  (verified by spot-check) but this is a long-standing landmine.

### `src/target/pcxt/CMakeLists.txt` user-program rules under-declare DEPENDS

- `add_custom_command(... DEPENDS ${source} ${PPAP_ROOT}/src/user/syscall.h ...)`
  is the maximum tracked.  Headers like `uclib.h`, `push.h`, the
  `pdb_*.h` family, and anything transitively reached through
  `syscall.h` (`common/dirent.h`, `common/fcntl.h`, `common/stat.h`,
  …) are not in the depends list.
- Editing any of those headers does *not* trigger user-program
  rebuild on incremental builds.

## Proposed fix order

1. **Fix the build dependency tracking first** (this note is about
   why).  Concretely: get `.S` compilations to emit `.d` files
   (CMake's `OBJECT_DEPENDS` per-source property, or
   `IMPLICIT_DEPENDS C ${file}` on the user-program custom commands,
   or pass `-MMD -MP -MF <out>.d` through the assembler flags and
   set the resulting depfile via `set_property(SOURCE ... PROPERTY
   OBJECT_DEPENDS ...)`).  Also widen the user-program
   `add_custom_command` DEPENDS lists to cover the headers actually
   `#include`d by each source.
2. With dep tracking fixed, every commit's `--clean`-vs-incremental
   discrepancy will collapse, so we'll actually see the latent
   crashes at commit time instead of weeks later.
3. Then re-investigate the post-prompt crash with confidence that
   the binaries reflect the source.  Top suspects to inspect with
   that confidence: vfork/execve stack hand-off path in
   `elf16_loader.c` + `trap.S` `exec_pending`, and the
   `vfs_entries.S` / `core_entries.S` non-reentrant save area.

## Useful debugging recipes

Run pcxt under host qemu with full execution trace:

```sh
qemu-system-i386 -machine pc -cpu 486 -m 1M -nographic -no-reboot \
  -drive file=build/pcxt/ppap_pcxt.img,format=raw,if=floppy \
  -serial file:/tmp/qemu.out \
  -d in_asm,cpu -D /tmp/qemu_trace.log
```

Then in the trace:

- `grep -n '^CS =2100' /tmp/qemu_trace.log` — find moments init/sh is
  running.
- The last `CS=2100` block is the LAST user-mode instruction before
  the crash.
- The next block with `CS=0000` while `SS=2100` is the first wild
  jump — its `EIP` is the corrupted IP that was popped from the
  user stack's IRET frame.

Inspecting the IRET frame at `int_debug_handler` entry via
`gdb-multiarch` attached to `qemu -s -S`:

```sh
hbreak *0x17f6b              # int_debug_handler entry, CS=0x1000:IP=0x7f6b
c
printf "IP=0x%04x CS=0x%04x FLAGS=0x%04x\n", \
  *(unsigned short*)($ss*16+$esp+0), \
  *(unsigned short*)($ss*16+$esp+2), \
  *(unsigned short*)($ss*16+$esp+4)
```
