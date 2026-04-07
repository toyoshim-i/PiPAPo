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
- per-process kernel stack storage for the parent's saved resume frame
- restore of that frame before the parent returns to user mode

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

- `sys_vfork()` saves the parent's 24-byte GP+IRET resume frame off the shared
   user stack and patches the saved AX slot with the child PID
- the child keeps using the shared user stack until `execve()` or `_exit()`
- `execve()` is allowed to rebuild that shared user stack for the new image
- when the parent becomes runnable again, the saved frame must be copied back
   to `user_SS:user_SP` before any path returns the parent to user mode

That last point turned out to be load-bearing.

During PC/XT bring-up, the first implementation restored the saved parent
frame on the syscall trap return path only.  That was not sufficient.  The
parent can also be resumed by the timer ISR path after scheduling, and that
path also ends in a user-mode `iret`.  If the timer return path skips the
restore, it can `iret` from the child's rewritten shared user stack and pop a
stale or corrupted `CS:IP`.

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
