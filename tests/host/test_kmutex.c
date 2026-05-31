/*
 * test_kmutex.c - Host tests for the sleepable process-owned kernel mutex
 */

#include "test_framework.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include "kernel/common/sync/kmutex.h"
#include "kernel/core/proc/proc.h"

pcb_t *current;
int test_in_irq_context;

static pcb_t proc_a;
static pcb_t proc_b;
static int panic_expected;
static int panic_seen;
static int switch_count;
static int wakeup_count;
static jmp_buf panic_env;
static void (*switch_hook)(void);

void panic(const char *fmt, ...) {
  (void)fmt;
  panic_seen = 1;
  if (panic_expected) longjmp(panic_env, 1);
  abort();
}

void sched_switch(void) {
  switch_count++;
  if (switch_hook) switch_hook();
}

void sched_wakeup(void *channel) {
  wakeup_count++;
  if (proc_a.state == PROC_BLOCKED && proc_a.wait_channel == channel) {
    proc_a.state = PROC_RUNNABLE;
    proc_a.wait_channel = NULL;
  }
  if (proc_b.state == PROC_BLOCKED && proc_b.wait_channel == channel) {
    proc_b.state = PROC_RUNNABLE;
    proc_b.wait_channel = NULL;
  }
}

static void reset_test(void) {
  proc_a = (pcb_t){.state = PROC_RUNNABLE};
  proc_b = (pcb_t){.state = PROC_RUNNABLE};
  current = &proc_a;
  test_in_irq_context = 0;
  panic_expected = 0;
  panic_seen = 0;
  switch_count = 0;
  wakeup_count = 0;
  switch_hook = NULL;
}

#define ASSERT_PANICS(stmt) do {                                           \
    panic_expected = 1;                                                    \
    if (setjmp(panic_env) == 0) {                                          \
      stmt;                                                                \
      ASSERT(0, #stmt " should panic");                                    \
    } else {                                                               \
      ASSERT(panic_seen, #stmt " should call panic");                      \
    }                                                                      \
    panic_expected = 0;                                                    \
  } while (0)

static void test_lock_unlock_tracks_owner(void) {
  kmutex_t mutex;

  reset_test();
  kmutex_init(&mutex);
  kmutex_lock(&mutex);
  ASSERT(mutex.owner == &proc_a, "lock should record owner");
  ASSERT(proc_a.kmutex_held == &mutex, "lock should join held list");

  kmutex_unlock(&mutex);
  ASSERT_NULL(mutex.owner);
  ASSERT_NULL(proc_a.kmutex_held);
  ASSERT_NULL(mutex.next_held);
  ASSERT_EQ(wakeup_count, 1);
}

static kmutex_t *handoff_mutex;

static void release_owner_during_switch(void) {
  current = &proc_a;
  kmutex_unlock(handoff_mutex);
  current = &proc_b;
  switch_hook = NULL;
}

static void test_waiter_acquires_after_handoff(void) {
  kmutex_t mutex;

  reset_test();
  kmutex_init(&mutex);
  kmutex_lock(&mutex);

  current = &proc_b;
  handoff_mutex = &mutex;
  switch_hook = release_owner_during_switch;
  kmutex_lock(&mutex);

  ASSERT_EQ(switch_count, 1);
  ASSERT(mutex.owner == &proc_b, "woken waiter should acquire mutex");
  ASSERT(proc_b.kmutex_held == &mutex, "waiter should track acquired mutex");
  ASSERT(proc_b.state == PROC_RUNNABLE, "woken waiter should be runnable");
  ASSERT_NULL(proc_b.wait_channel);
}

static void test_release_owned_wakes_waiter(void) {
  kmutex_t first;
  kmutex_t second;

  reset_test();
  kmutex_init(&first);
  kmutex_init(&second);
  kmutex_lock(&first);
  kmutex_lock(&second);
  proc_b.state = PROC_BLOCKED;
  proc_b.wait_channel = &first;

  kmutex_release_owned(&proc_a);

  ASSERT_NULL(proc_a.kmutex_held);
  ASSERT_NULL(first.owner);
  ASSERT_NULL(second.owner);
  ASSERT_NULL(first.next_held);
  ASSERT_NULL(second.next_held);
  ASSERT(proc_b.state == PROC_RUNNABLE, "owner death should wake waiter");
  ASSERT_NULL(proc_b.wait_channel);
  ASSERT_EQ(wakeup_count, 2);
}

static void test_recursive_lock_panics(void) {
  kmutex_t mutex;

  reset_test();
  kmutex_init(&mutex);
  kmutex_lock(&mutex);
  ASSERT_PANICS(kmutex_lock(&mutex));
}

static void test_unlock_by_non_owner_panics(void) {
  kmutex_t mutex;

  reset_test();
  kmutex_init(&mutex);
  kmutex_lock(&mutex);
  current = &proc_b;
  ASSERT_PANICS(kmutex_unlock(&mutex));
}

static void test_irq_context_lock_unlock_panics(void) {
  kmutex_t mutex;

  reset_test();
  kmutex_init(&mutex);
  test_in_irq_context = 1;
  ASSERT_PANICS(kmutex_lock(&mutex));
  ASSERT_PANICS(kmutex_unlock(&mutex));
}

int main(void) {
  printf("=== test_kmutex ===\n");
  RUN_TEST(test_lock_unlock_tracks_owner);
  RUN_TEST(test_waiter_acquires_after_handoff);
  RUN_TEST(test_release_owned_wakes_waiter);
  RUN_TEST(test_recursive_lock_panics);
  RUN_TEST(test_unlock_by_non_owner_panics);
  RUN_TEST(test_irq_context_lock_unlock_panics);
  TEST_SUMMARY();
}
