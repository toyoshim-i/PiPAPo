# RISC-V Double-Free / Crash Investigation

Status: **root cause found, partial fix applied** (2026-03-25)

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

## Root Cause

### Missing stack pointer relocation in RISC-V vfork

RISC-V has no hardware kernel/user stack split (unlike ARM's MSP/PSP or
m68k's SSP/USP).  Kernel and user code share a single `stack_page`.

When `sys_vfork()` creates a child:
1. `memcpy(child_stack, parent_stack, PAGE_SIZE)` — byte-for-byte copy
2. Saved registers (s0/fp, callee-saved) in the trap frame still point
   to the **parent's** stack page
3. Stack-spilled frame pointers and local pointer variables also
   reference the parent's page

After vfork returns in the child, `s0` (frame pointer) still points to
the parent's page.  All frame-pointer-relative accesses (local variables,
argv construction, function arguments) go to the **parent's** page
instead of the child's copy.  The argv array passed to `execve` ends up
containing stale data from the parent's stack (kernel return addresses,
saved registers), not the actual command strings.

### Backtrace confirmation

Added RISC-V frame pointer backtrace to the exception handler.  The crash
call chain:

```
_trap_entry → syscall_dispatch → sys_execve → do_execve → elf_load → strlen
```

Debug argv dump showed:
```
argv[0]=0x808f78ef ""      ← empty string (stale stack data)
argv[1]=0x8006277f          ← kernel ROM address (stale saved register)
argv[3]=0x00000008          ← crash address
```

After applying the stack relocation fix:
```
argv[0]=0x808f8280 "/bin/ls"   ← correct
argv[1]=0x808f8c3b "/"         ← correct
argc=2                          ← correct
```

### Why RISC-V specific

- **ARM**: Hardware MSP/PSP split.  `stack_page` is kernel-only (MSP).
  User code runs on PSP which points into `user_pages[]` (shared in
  vfork at the same address).  No relocation needed.
- **m68k**: SSP/USP split.  vfork already copies and relocates the
  `user_stack_page` separately, including a6/fp fixup.
- **RISC-V**: No stack split.  Single `stack_page` holds kernel trap
  frames AND user stack frames.

### Use-after-free in execve (also fixed)

`sys_execve` freed `old_stack` (line 1745) while still executing on it.
ARM can do this because the kernel runs on MSP (not PSP).  RISC-V and
m68k run on the same stack — freeing it is a use-after-free.

Fixed by deferring the free to `trap.S` after the SP switch (same
pattern m68k already used).

## Fixes Applied

1. **Deferred stack free in execve** — `exec_old_stack` global
   (shared by m68k and RISC-V); freed in trap.S after SP switch.

2. **Brute-force stack page relocation in vfork** — scans every
   aligned word in the child's copied stack page; shifts values in
   `[parent_base, parent_base+PAGE_SIZE)` by delta.  This fixes
   frame pointers, return addresses, and spilled pointer locals.

3. **High-first single-page allocation** — `page_alloc()` picks the
   highest-address free page, segregating stacks (high) from
   contiguous ELF images (low).  Reduces fragmentation.

4. **Stack backtrace on exception** — frame pointer chain walk in
   `riscv_exception_handler` and `page_free` (double-free).
   Requires `-fno-omit-frame-pointer`.

## Remaining Issues

### Parent shell crash after child exits

After `ls` exits (with "out of memory"), the shell (pid=2) crashes.
The brute-force relocation may have false positives — integer values
that coincidentally fall in the parent's stack page range get shifted,
corrupting the child's stack data.  When the child runs before execve,
this corrupted data could cause subtle problems.

### `ls: out of memory`

Busybox `ls` now executes (argv correct) but fails to allocate memory.
musl apps work fine on ARM with 4KB stacks, so this is not a stack
size issue.  Likely cause: brk/malloc failure from the relocation
side effects, or a separate issue with the child's heap setup.

## Proper Fix: mscratch-based kernel/user stack split

The brute-force relocation is fragile (false positives).  The proper
fix is a software kernel/user stack split using the `mscratch` CSR:

1. On trap entry: `csrrw sp, mscratch, sp` — atomically swap user SP
   and kernel SP
2. Each process gets separate kernel stack (1 page) and user stack
   (in `user_pages[]`)
3. vfork shares `user_pages[]` naturally (same address in parent/child)
4. No relocation needed, no deferred-free needed

This is the standard approach used by xv6 and Linux on RISC-V.

## Docker Build Fixes (committed earlier)

1. **Toolchain path**: `qemu_rv32/CMakeLists.txt` respects `PICO_TOOLCHAIN_PATH`
2. **Hard-float libgcc fallback**: `user.cmake` detects and falls back
3. **Rogue K&R fix**: `-std=gnu11` for GCC 15
4. **Rogue PIE/strip**: conditional `-pie` vs `--emit-relocs`
5. **Busybox oldconfig**: `yes ""` for non-interactive
6. **r68k test skip**: skip when compiler unavailable
