#ifndef PPAP_TESTS_HOST_INCLUDE_KERNEL_CORE_PROC_SCHED_H
#define PPAP_TESTS_HOST_INCLUDE_KERNEL_CORE_PROC_SCHED_H

void sched_switch(void);
void sched_sleep_current_unlock(void *channel, unsigned int lock_num,
                                unsigned int saved);
void sched_wakeup(void *channel);

#endif /* PPAP_TESTS_HOST_INCLUDE_KERNEL_CORE_PROC_SCHED_H */
