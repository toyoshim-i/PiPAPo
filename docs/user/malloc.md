# malloc — Userland heap allocator

Small best-fit allocator in uclib, operating over a caller-supplied
static pool.  Designed for PPAP user processes on targets with tight
memory budgets (the ia16 pcxt port shares a single 64 KB segment
between text, data, bss, and stack, so stack vectors are expensive
and a compact heap buys a lot of room back).

> See also: [src/user/lib/alloc.c](/src/user/lib/alloc.c),
> [tests/host/test_alloc.c](/tests/host/test_alloc.c).
> Inspired by the kernel's [page_alloc](/src/kernel/core/mm/page_alloc.c)
> policy (address-sorted free list, best-fit, always-coalesced on
> free) but with metadata stored inline in the pool rather than in
> an external array — appropriate when caller-owned storage is a
> small contiguous block.

## API

```c
#include "lib/uclib.h"

void  uc_heap_init(void *pool, size_t size);
void *malloc(size_t size);
void  free(void *ptr);
```

- `uc_heap_init(pool, size)` — seed the heap over the caller's
  storage.  Idempotent: a second call resets the free list and
  re-seeds the whole pool (caller must have freed all live
  allocations first).
- `malloc(size)` — returns a pointer to `size` bytes of usable
  memory, or `NULL` on OOM, on `size == 0`, or when the request
  exceeds the 16-bit internal size field (> 0xFFFC bytes).
- `free(ptr)` — NULL-safe; double-free is undefined behaviour.

Only one pool per process.  The allocator has no global storage
beyond a single free-list head pointer.

## Usage pattern

```c
static char my_heap_pool[1024];

int main(int argc, char *argv[]) {
  uc_heap_init(my_heap_pool, sizeof(my_heap_pool));

  char *buf = malloc(128);
  if (!buf) { /* handle OOM */ }
  /* ... use buf ... */
  free(buf);

  return 0;
}
```

Sizing the pool: add up the largest expected *concurrent* set of
allocations, include 4 B header per allocation, and leave some
headroom.  Example from pile: op path holds ~900 B of live
allocations while `refresh_panes` pulls another ~220 B
concurrently, so pile uses a 1536-byte pool.

## Layout

Every block carries a **4-byte header**:

```c
typedef struct uc_block {
  uint16_t size;    /* payload bytes, not counting this header */
  uint16_t flags;   /* bit 0 = free (informational; not relied on
                     * for list membership) */
} uc_block_t;
```

Free blocks overlay a doubly-linked-list node on the first bytes
of their payload:

```c
typedef struct uc_free {
  uc_block_t hdr;
  struct uc_free *next;   /* free block at higher address, or NULL */
  struct uc_free *prev;   /* free block at lower address, or NULL  */
} uc_free_t;
```

When a block is allocated the `next` / `prev` area is handed back
to the caller as ordinary user data, so **live allocations carry
zero overhead beyond the 4-byte header**.

## Algorithm

### Invariants (mirrored from `page_alloc`)

- The free list is sorted by address, ascending.
- No two free blocks are physically adjacent — every `free`
  eagerly merges with any adjacent free neighbor.
- `hdr.size > 0` on every block (no zero-size blocks).

### `malloc(n)` — best-fit

1. Round `n` up to the machine word and to the minimum free-block
   payload size.
2. Walk the free list.  Track the smallest block whose `size >= n`.
   Short-circuit on a perfect fit.
3. If no block fits: return `NULL`.
4. If the chosen block's remainder (`size - n`) can stand as its
   own free block (header + minimum free payload), **split**:
   - Shrink the chosen block's `size` to `n`.
   - Create a new free header at `base + sizeof(hdr) + n`.
   - Replace the old free-list entry with the new split block
     (same address position in the sorted list).
5. Otherwise absorb the slack into the allocation; remove the
   block from the free list.
6. Clear the free flag and return `(char *)block + sizeof(hdr)`.

### `free(ptr)` — always-coalesce

1. Header is at `ptr - 4`.  Mark free.
2. Find the insertion point in the address-sorted free list
   (O(free_count) walk).  Result: `list_prev` (highest-addr free
   block below `ptr`) and `list_next` (lowest above).
3. If `list_prev + sizeof(hdr) + list_prev->size == block_ptr`,
   the previous free block is physically adjacent: **merge**
   into `list_prev`.  (Sorted-list invariant guarantees no other
   free block can be the physical predecessor.)
4. Otherwise insert the new free block between `list_prev` and
   `list_next`.
5. If the new (possibly-merged) block's tail == `list_next`'s
   start, merge with `list_next` as well.

The "sorted-list implies physical neighbors" property makes
backward coalesce O(1) once the insertion point is known — same
trick `page_alloc` uses with its run table.

## Limits

- **16-bit `size` field** caps a single allocation at 64 KB − 4.
  Fits the ia16 segment naturally; on 32-bit / 64-bit targets the
  cap only matters if you ask for one giant buffer (pile's
  largest allocation is 256 bytes).
- **Minimum block size** is 4 B header + 4 B (ia16) / 8 B (32-bit)
  / 16 B (64-bit) free-node payload.  Requests smaller than the
  free-node minimum are rounded up so a freed block can always
  rejoin the free list.
- **Best-fit walk** is O(free_block_count) per `malloc`.  For
  small pools (hundreds of bytes, a dozen or so live blocks) this
  is trivial.
- **Single process, single thread.**  The allocator has no locking
  — matches PPAP's cooperative-vfork user-process model.

## When to use (and when not)

Use `malloc` when:

- You have variable-size transient buffers whose lifetime is a
  single function / operation.
- Your per-process stack budget is tight and the alternatives
  would be 100+ B stack locals (path buffers, dirent structs,
  I/O buffers).
- Multiple functions share the same scratch need at different
  times, so a fixed-size static buffer per call site would waste
  BSS.

Prefer something else when:

- You have a fixed set of same-sized objects — use the kernel's
  [`kmem`](/src/kernel/core/mm/kmem.h)-style intrusive free list
  pattern (zero overhead, O(1) alloc/free).  For user space a
  small static array + free bitmap is usually simpler.
- Allocations are long-lived relative to the process lifetime —
  static BSS is cheaper than heap overhead.
- You need cross-process shared memory — PPAP has no such thing,
  but the brk/mmap syscalls are there for single-process large
  blocks.

## Testing

`tests/host/test_alloc.c` exercises the allocator on the host
with no stubs (the implementation has no hardware or syscall
dependencies).  Covers:

- init / alloc-within-pool / size-zero / OOM / NULL-safe free /
  tiny-pool-noop / re-init reset.
- Alloc-free-alloc same-size reuse.
- Split leaves usable remainder.
- Forward, backward, and both-sided coalesce.
- Fragment recovery (alternating frees leave the pool fully
  merged).
- LIFO workload pattern matching pile's op path (50 iterations,
  verifies no cross-allocation corruption).

6540 assertions, 0 failures.  Runs as part of `./scripts/test.sh`.
