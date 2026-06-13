/*
 * kmutex.h — Sleepable process-owned kernel mutex
 *
 * kmutex_t is for process-context paths that may block, call drivers, call
 * filesystems, or allocate memory.  It is not valid from hardware IRQ
 * context.
 */

#ifndef PPAP_KERNEL_COMMON_SYNC_KMUTEX_H
#define PPAP_KERNEL_COMMON_SYNC_KMUTEX_H

struct pcb;

typedef struct kmutex {
  struct pcb *volatile owner;
  struct kmutex *next_held;
} kmutex_t;

void kmutex_init(kmutex_t *m);
void kmutex_lock(kmutex_t *m);
void kmutex_release_owned(struct pcb *p);
int kmutex_try_lock(kmutex_t *m);
void kmutex_unlock(kmutex_t *m);

#endif /* PPAP_KERNEL_COMMON_SYNC_KMUTEX_H */
