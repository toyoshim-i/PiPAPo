/*
 * kstack_region.c — Xtensa fixed kernel-stack backing storage.
 *
 * ESP-IDF owns the final linker script for xtensa_cc, so Xtensa reserves the
 * common kstack region as aligned kernel BSS instead of spelling it out in a
 * target .ld file.
 */

#include "kernel/common/config.h"
#include "kernel/core/proc/kstack.h"

char __kstack_region_base[PROC_KSTACK_IDLE_SIZE +
                          (PROC_MAX - 1u) * PROC_KSTACK_SIZE]
    __attribute__((aligned(16)));
