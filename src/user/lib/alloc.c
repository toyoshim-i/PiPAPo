/*
 * alloc.c — user-space heap allocator.
 *
 * Best-fit address-sorted free list with inline 4-byte block headers.
 * Mirrors the kernel page allocator's always-coalesced invariant.
 *
 * malloc / free follow standard semantics; uc_heap_init seeds the
 * allocator with a caller-owned static pool (no syscall, idempotent).
 *
 * Self-contained — no other PPAP libc TU is needed to link this.
 * tests/host/test_uc_heap.c exercises the allocator directly on the
 * host without stubs.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

void uc_heap_init(void *pool, size_t size);

#define UC_FLAG_FREE 0x0001u

typedef struct uc_block {
  uint16_t size; /* payload bytes excluding this header */
  uint16_t flags;
} uc_block_t;

/* Free blocks overlay this struct on their header + first payload bytes.
 * Once allocated, next/prev are clobbered by user data — no runtime cost. */
typedef struct uc_free {
  uc_block_t hdr;
  struct uc_free *next; /* higher-address free block (NULL at tail) */
  struct uc_free *prev; /* lower-address free block (NULL at head)  */
} uc_free_t;

#define UC_FREE_PAYLOAD (sizeof(uc_free_t) - sizeof(uc_block_t))

/* Align `n` up to the natural machine word so pointers stored in a
 * free block's payload area are aligned.  Using sizeof(void *) gives
 * 2 on ia16 and 4 on 32-bit targets, matching our free-node layout. */
#define UC_ALIGN (sizeof(void *))
#define UC_ALIGN_UP(n) (((n) + (UC_ALIGN - 1)) & ~(UC_ALIGN - 1))

/* Smallest payload size we can actually hold as a free block (the
 * free-node prev/next pointers go here when the block returns to the
 * free list).  Any block with `size` less than this never appears in
 * the free list; over-sized mallocs that would split off a tinier
 * remainder keep the remainder as part of the allocation instead. */
#define UC_MIN_FREE_PAYLOAD UC_ALIGN_UP(UC_FREE_PAYLOAD)

static uc_free_t *uc_heap_head;

static uc_block_t *block_next_physical(uc_block_t *b) {
  return (uc_block_t *)((char *)b + sizeof(uc_block_t) + b->size);
}

static int blocks_adjacent(uc_block_t *low, uc_block_t *high) {
  return block_next_physical(low) == high;
}

static void free_list_insert_after(uc_free_t *prev, uc_free_t *node,
                                   uc_free_t *next) {
  node->prev = prev;
  node->next = next;
  if (prev)
    prev->next = node;
  else
    uc_heap_head = node;
  if (next) next->prev = node;
}

static void free_list_remove(uc_free_t *node) {
  if (node->prev)
    node->prev->next = node->next;
  else
    uc_heap_head = node->next;
  if (node->next) node->next->prev = node->prev;
}

void uc_heap_init(void *pool, size_t size) {
  uc_heap_head = 0;
  if (!pool) return;

  /* Need at least header + minimum payload to form one free block. */
  size_t min = sizeof(uc_block_t) + UC_MIN_FREE_PAYLOAD;
  if (size < min) return;
  if (size > 0xFFFFu + sizeof(uc_block_t)) size = 0xFFFFu + sizeof(uc_block_t);

  uc_free_t *b = (uc_free_t *)pool;
  b->hdr.size = (uint16_t)(size - sizeof(uc_block_t));
  b->hdr.flags = UC_FLAG_FREE;
  b->next = 0;
  b->prev = 0;
  uc_heap_head = b;
}

void *malloc(size_t size) {
  if (!uc_heap_head || size == 0) return 0;
  if (size > 0xFFFCu) return 0; /* leave room for the sizeof(uc_free_t) check */

  size_t want = UC_ALIGN_UP(size);
  if (want < UC_MIN_FREE_PAYLOAD) want = UC_MIN_FREE_PAYLOAD;
  if (want > 0xFFFCu) return 0;

  /* Best-fit scan.  Short-circuit on perfect match. */
  uc_free_t *best = 0;
  for (uc_free_t *f = uc_heap_head; f; f = f->next) {
    if (f->hdr.size < want) continue;
    if (!best || f->hdr.size < best->hdr.size) {
      best = f;
      if (f->hdr.size == want) break;
    }
  }
  if (!best) return 0;

  /* Split only if the remainder can stand as its own free block
   * (header + UC_MIN_FREE_PAYLOAD).  Otherwise absorb the extra. */
  size_t remainder = (size_t)best->hdr.size - want;
  if (remainder >= sizeof(uc_block_t) + UC_MIN_FREE_PAYLOAD) {
    uc_free_t *split =
        (uc_free_t *)((char *)best + sizeof(uc_block_t) + want);
    split->hdr.size = (uint16_t)(remainder - sizeof(uc_block_t));
    split->hdr.flags = UC_FLAG_FREE;
    best->hdr.size = (uint16_t)want;
    /* Replace `best` with `split` in the free list (same address
     * position: split sits just above `best` but `best` will become
     * used immediately; the list must stay address-sorted). */
    split->prev = best->prev;
    split->next = best->next;
    if (best->prev)
      best->prev->next = split;
    else
      uc_heap_head = split;
    if (best->next) best->next->prev = split;
  } else {
    free_list_remove(best);
  }

  best->hdr.flags = 0;
  return (char *)best + sizeof(uc_block_t);
}

void free(void *ptr) {
  if (!ptr) return;
  uc_free_t *b = (uc_free_t *)((char *)ptr - sizeof(uc_block_t));
  b->hdr.flags = UC_FLAG_FREE;

  /* Find insertion point in the address-sorted free list. */
  uc_free_t *next = uc_heap_head;
  uc_free_t *prev = 0;
  while (next && (uintptr_t)next < (uintptr_t)b) {
    prev = next;
    next = next->next;
  }

  /* Coalesce with list_prev if they share a physical boundary. */
  if (prev && blocks_adjacent(&prev->hdr, &b->hdr)) {
    prev->hdr.size =
        (uint16_t)(prev->hdr.size + sizeof(uc_block_t) + b->hdr.size);
    b = prev; /* b now refers to the merged block; already in list */
  } else {
    free_list_insert_after(prev, b, next);
  }

  /* Coalesce with list_next (post-merge b) if they share a boundary. */
  if (next && blocks_adjacent(&b->hdr, &next->hdr)) {
    b->hdr.size =
        (uint16_t)(b->hdr.size + sizeof(uc_block_t) + next->hdr.size);
    b->next = next->next;
    if (next->next) next->next->prev = b;
  }
}
