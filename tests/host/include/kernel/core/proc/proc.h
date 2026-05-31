#ifndef PPAP_TESTS_HOST_INCLUDE_KERNEL_CORE_PROC_PROC_H
#define PPAP_TESTS_HOST_INCLUDE_KERNEL_CORE_PROC_PROC_H

struct kmutex;

typedef enum proc_state {
  PROC_FREE = 0,
  PROC_RUNNABLE,
  PROC_BLOCKED,
} proc_state_t;

typedef struct pcb {
  proc_state_t state;
  void *wait_channel;
  struct kmutex *kmutex_held;
} pcb_t;

extern pcb_t *current;

#endif /* PPAP_TESTS_HOST_INCLUDE_KERNEL_CORE_PROC_PROC_H */
