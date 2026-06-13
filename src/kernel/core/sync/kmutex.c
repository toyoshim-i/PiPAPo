/*
 * kmutex.c — Sleepable process-owned kernel mutex
 */

#include "kernel/common/sync/kmutex.h"

#include <stddef.h>

#include "kernel/common/spinlock.h"
#include "kernel/core/panic.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/proc/sched.h"

static void kmutex_link_held(kmutex_t *m) {
  m->next_held = current->kmutex_held;
  current->kmutex_held = m;
}

static void kmutex_unlink_held(pcb_t *owner, kmutex_t *m) {
  kmutex_t **pp = &owner->kmutex_held;

  while (*pp) {
    if (*pp == m) {
      *pp = m->next_held;
      m->next_held = NULL;
      return;
    }
    pp = &(*pp)->next_held;
  }
}

static void kmutex_require_process_context(const char *op) {
  if (arch_in_irq_context()) panic("%s in IRQ context\n", op);
  if (!current) panic("%s without current\n", op);
}

void kmutex_init(kmutex_t *m) {
  if (!m) return;
  m->owner = NULL;
  m->next_held = NULL;
}

void kmutex_lock(kmutex_t *m) {
  if (!m) panic("kmutex_lock(NULL)\n");
  kmutex_require_process_context("kmutex_lock");

  for (;;) {
    uint32_t saved = spin_lock_irqsave(SPIN_PROC);

    if (!m->owner) {
      m->owner = current;
      kmutex_link_held(m);
      spin_unlock_irqrestore(SPIN_PROC, saved);
      return;
    }

    if (m->owner == current) {
      spin_unlock_irqrestore(SPIN_PROC, saved);
      panic("recursive kmutex_lock\n");
    }

    sched_sleep_current_unlock(m, SPIN_PROC, saved);
  }
}

int kmutex_try_lock(kmutex_t *m) {
  if (!m) panic("kmutex_try_lock(NULL)\n");
  kmutex_require_process_context("kmutex_try_lock");

  uint32_t saved = spin_lock_irqsave(SPIN_PROC);

  if (!m->owner) {
    m->owner = current;
    kmutex_link_held(m);
    spin_unlock_irqrestore(SPIN_PROC, saved);
    return 1;
  }

  if (m->owner == current) {
    spin_unlock_irqrestore(SPIN_PROC, saved);
    panic("recursive kmutex_try_lock\n");
  }

  spin_unlock_irqrestore(SPIN_PROC, saved);
  return 0;
}

void kmutex_unlock(kmutex_t *m) {
  if (!m) panic("kmutex_unlock(NULL)\n");
  kmutex_require_process_context("kmutex_unlock");

  uint32_t saved = spin_lock_irqsave(SPIN_PROC);
  if (m->owner != current) {
    spin_unlock_irqrestore(SPIN_PROC, saved);
    panic("kmutex_unlock by non-owner\n");
  }

  kmutex_unlink_held(current, m);
  m->owner = NULL;
  spin_unlock_irqrestore(SPIN_PROC, saved);
  sched_wakeup(m);
}

void kmutex_release_owned(pcb_t *p) {
  if (!p) return;

  for (;;) {
    uint32_t saved = spin_lock_irqsave(SPIN_PROC);
    kmutex_t *m = p->kmutex_held;

    if (!m) {
      spin_unlock_irqrestore(SPIN_PROC, saved);
      return;
    }

    p->kmutex_held = m->next_held;
    m->next_held = NULL;
    if (m->owner == p) m->owner = NULL;
    spin_unlock_irqrestore(SPIN_PROC, saved);
    sched_wakeup(m);
  }
}
