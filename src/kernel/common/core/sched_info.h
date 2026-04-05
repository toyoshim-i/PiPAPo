/*
 * sched_info.h --- Scheduler statistics shared with VFS (procfs)
 *
 * Per-CPU tick counters for /proc/stat and /proc/uptime.
 * Defined in sched.c (core module).
 */

#ifndef PPAP_KERNEL_COMMON_CORE_SCHED_INFO_H
#define PPAP_KERNEL_COMMON_CORE_SCHED_INFO_H

#include <stdint.h>

extern uint32_t cpu_user_ticks[2];
extern uint32_t cpu_system_ticks[2];
extern uint32_t cpu_idle_ticks[2];

#endif /* PPAP_KERNEL_COMMON_CORE_SCHED_INFO_H */
