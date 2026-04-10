# Proposal: No User-Stack Copy on `vfork()`

## Summary

PPAP currently uses target-specific stack-copy logic on some architectures
to make `vfork()` safe.  The PC/XT i16 port demonstrated a simpler model:

- the child shares the parent's user stack, as `vfork()` semantics already imply
- the parent is blocked until the child calls `execve()` or `_exit()`
- only the parent's vulnerable resume frame is saved out-of-line
- that saved frame is restored when the parent resumes

This document proposes treating that model as the preferred long-term
`vfork()` design for PPAP.

The goal is not to change `fork()`.  This is about `vfork()` only.

## Motivation

The existing stack-copy approach has real costs:

- extra allocation during `vfork()`
- extra copy time on the hottest process-spawn path
- more target-specific complexity
- more ways for parent/child stack remapping bugs to appear

For true `vfork()`, the child is supposed to borrow the parent's address
space briefly and then either `execve()` or `_exit()`.  Copying the full user
stack works, but it is more than the semantics require.

The i16 implementation showed that the actual hazard is narrower:

- the child re-enters the kernel and overwrites the parent's saved return frame
- the parent later resumes with corrupted state unless that frame was preserved

Once that frame is saved elsewhere, a full user-stack copy is unnecessary.

## i16 Result

The PC/XT i16 port now uses:

- shared user stack during `vfork()`
- a reserved 34-byte slot in the per-process kernel stack (fixed 2 KB slots)
   for the parent's saved GP+IRET frame (24B) and vfork stub frame (10B)
- restore of that frame before the parent returns to user mode, on both the
   syscall and timer ISR return paths

That was enough to make:

- `init`
- `vfork()`
- `execve("/bin/sh")`
- shell startup

work correctly without allocating a separate child user stack.

This is the first concrete proof inside PPAP that a "save minimal parent
resume state, do not copy the whole user stack" approach is viable.

## i16 Technical Detail

The working i16 design is more specific than just "save a frame somewhere":

- `sys_vfork()` saves a 34-byte region from the parent's shared user stack
   to the parent's own kernel stack, below `trap_ksp`.  The 34 bytes cover:
   - 24 bytes: the GP+IRET frame (ES, DS, BP, DI, SI, DX, CX, BX, AX,
     IP, CS, FLAGS) — popped by trap.S restore + `iret`
   - 10 bytes: the vfork syscall stub's callee-saved registers and return
     address (saved DI, SI, BX, BP + `call vfork` return IP) — popped by
     `SYSCALL_RET` after `iret`
   The AX slot (offset 16) in the saved copy is patched with the child PID
   so the parent sees the correct return value when resumed.
- the child keeps using the shared user stack until `execve()` or `_exit()`
- `execve()` is allowed to rebuild that shared user stack for the new image
- when the parent becomes runnable again, `i16_vfork_restore_frame()` copies
   the saved 34 bytes back to `user_SS:user_SP` before any path returns the
   parent to user mode

### What must be saved (and why)

The user stack at the time of the `int $0x30` in the vfork stub looks like:

```
  [user_SP+0 ]  ES DS BP DI SI DX CX BX AX IP CS FLAGS   ← 24B GP+IRET
  [user_SP+24]  saved DI SI BX BP  (vfork stub pushw's)   ← 10B stub frame
  [user_SP+34]  return address from `call vfork`
  [user_SP+36]  parent's C caller frame (locals, saved regs)
```

The child returns from vfork with AX=0 and `SYSCALL_RET` pops the stub
frame (bytes 24–33), returning to the parent's C caller.  From there, the
child must call only `execve()` or `_exit()`.  Even a bare `execve(path,
argv, envp)` call pushes 3 args + return address + syscall stub prologue
onto the shared stack — overwriting the 10 bytes at `[user_SP+24..+33]`.

The kernel therefore saves 34 bytes (not just 24):

- **Lower side** `[user_SP+0..+23]` (24B): the GP+IRET frame.  The child's
  `execve` syscall trap pushes a new GP+IRET frame here, overwriting the
  parent's original values.
- **Upper side** `[user_SP+24..+33]` (10B): the vfork stub's callee-saved
  regs and return address.  The child's function calls unavoidably overwrite
  this area too.

The region above `[user_SP+34]` (the parent's C caller frame) is NOT saved.
The child must not touch it — doing so violates POSIX vfork semantics.
In particular, calling any function other than `execve()` or `_exit()` in
the vfork child path is undefined behavior.

### Kernel stack layout

Every kernel entry path (`trap.S` INT 30h handler and `switch.S` timer ISR)
reserves a 34-byte slot at a fixed position in the kernel stack:

```
  ktop - 2   user_SS   (pushed by entry code)
  ktop - 4   user_SP
  ktop - 38  34-byte vfork-save slot  (subw $34, %sp)
  ...        C call chain frames below
```

`sys_vfork()` writes the parent's saved frame into the slot at
`[ktop - 38, ktop - 4)`.  The slot is wasted (34 bytes) on the common
non-vfork path but keeps the layout uniform.

### AX return-value guard

After the syscall handler returns, `trap.S` normally writes the return value
into the saved AX slot on the user stack.  For a vfork parent whose child is
still running on the shared user stack, that write would clobber the child's
AX = 0.  `i16_trap_should_skip_ret_store()` checks `current->vfork_frame_saved`
and skips the AX write in that case.

### Two restore paths

During PC/XT bring-up, the first implementation restored the saved parent
frame on the syscall trap return path only.  That was not sufficient.  The
parent can also be resumed by the timer ISR path after scheduling, and that
path also ends in a user-mode `iret`.  If the timer return path skips the
restore, it can `iret` from the child's rewritten shared user stack and pop a
stale or corrupted `CS:IP`.

Both `trap.S` (line ~206) and `switch.S` (line ~161) now call
`i16_vfork_restore_frame()` before the final `iret`.  The restore function
checks `current->vfork_frame_saved`, and if set, copies the 34 bytes from the
kernel stack slot back to the user stack via `mem_region_page_write()`, then
clears the flag.

So the actual invariant is:

- every kernel exit path that can resume a blocked `vfork()` parent must run
   the parent-frame restore before restoring user registers and executing the
   final return instruction (`iret`, `rte`, equivalent trap return, etc.)

This is useful guidance for other targets.  The right question is not only
"what parent state must be saved?" but also "which exact kernel exit paths can
return that parent to user mode, and do all of them restore the saved state?"

## Proposed Model

### Rule

For `vfork()`:

1. The child shares the parent's user address space, including the user stack.
2. The parent is marked blocked immediately after the child is made runnable.
3. The kernel preserves only the parent state that the child's syscall/exec
   path would overwrite before the parent resumes.
4. When the child calls `execve()` or `_exit()`, the parent is made runnable.
5. Before returning the parent to user mode, the kernel restores the saved
   parent resume state.

### Non-goal

Do not apply this to `fork()`.

`fork()` still needs independent writable state.  This proposal is only about
removing unnecessary copying from the `vfork()` path.

## What Must Be Preserved

The exact saved state is architecture-dependent, but the principle is the same:

- preserve the parent frame that the child will overwrite by entering the kernel
- do not preserve unrelated user memory just because it happens to live nearby

Examples:

- i16: the interrupted user GP+return frame on the shared user stack
- m68k: the parent trap/return frame, if the child would overwrite it
- RISC-V: the user-return frame or other resume metadata, depending on trap layout
- ARM: likely less urgent if the kernel already runs on a separate stack, but the
  same analysis still applies

## Conditions for Using This Design

This model is safe only if all of the following are true:

1. The parent cannot run concurrently with the child while the address space is shared.
2. The child is restricted to `execve()` or `_exit()` style behavior expected of `vfork()`.
3. The architecture can identify exactly which parent resume state is at risk.
4. The kernel has a safe place to store that parent state temporarily.
5. The restore point is well-defined and runs before the parent returns to user mode on every possible resume path.

If a target cannot satisfy those conditions cleanly, it should keep its current
stack-copy implementation.

## Expected Benefits

- simpler `vfork()` implementation
- less target-specific allocation/remap logic
- lower spawn overhead
- behavior closer to actual `vfork()` semantics
- fewer bugs caused by partial stack-copy or pointer-remap mistakes

## Risks

- restoring the wrong frame or restoring at the wrong time can corrupt the parent
- restoring the frame on one resume path but forgetting another can leave a latent bug that appears only when scheduling changes
- some targets may have more than one vulnerable frame, not just one
- signal delivery, syscall restart, and tracing can complicate the restore path
- targets that mix kernel and user stack usage may need extra care

Because of that, this should be adopted target by target, not by a blind
global refactor.

## Suggested Rollout

### Phase 1: Document the pattern

- Treat the i16 implementation as the reference example.
- Make clear that the saved-state set is minimal and architecture-specific.

### Phase 2: Audit each target

For each architecture:

1. Identify where the parent's user-return state lives at `vfork()` time.
2. Identify what the child will overwrite before `execve()` or `_exit()`.
3. Determine whether saving just that state is sufficient.
4. Enumerate every kernel exit path that can resume the parent and verify all of them restore the saved state before returning to user mode.

### Phase 3: Convert one target at a time

Good candidates are targets that currently copy a user stack only to protect
the parent's return path, not because the kernel fundamentally requires a
separate child stack object.

For each conversion, treat "resume-path coverage" as a first-class checklist
item, not an afterthought.

### Phase 4: Remove obsolete helpers

If multiple targets converge on this model, the generic stack-copy helpers can
be narrowed or removed from the `vfork()` path.

## Recommendation

Adopt the following policy:

- default design goal for `vfork()` is "no user-stack copy"
- per-target fallback is allowed where trap/frame layout still requires it

The i16 port is now evidence that PPAP does not need to assume stack copy is
inherent to `vfork()`.
