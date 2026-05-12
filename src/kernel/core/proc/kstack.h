/*
 * kstack.h — per-process kernel-stack init + overflow sentinels.
 *
 * proc_init / proc_alloc call proc_kstack_init_slot() to set up the
 * per-process kernel stack pointer (PCB.kernel_sp) for a given slot,
 * and proc_kstack_plant_canary() to plant the slot's overflow
 * sentinel.  proc_check_kstack_canary_panic() walks every slot's
 * sentinel from end-of-syscall paths; on mismatch it logs and halts.
 *
 * Implementation in kstack.c uses the fixed-region model for every supported
 * architecture.
 */

#ifndef PPAP_KERNEL_CORE_PROC_KSTACK_H
#define PPAP_KERNEL_CORE_PROC_KSTACK_H

#include <stdint.h>

#include "kernel/common/core/proc_info.h"

/* Base of the per-process kernel-stack region.
 * Defined by target linker scripts (e.g. pcxt_kernel.ld, qemu.ld) or by
 * arch-owned storage on targets whose final linker script is external.
 * Declared here so the overlays do not need an `extern` in their .c. */
extern char __kstack_region_base[];

void proc_kstack_init_slot(pcb_t *p, uint32_t slot_idx);
void proc_kstack_plant_canary(uint32_t slot_idx);

#ifdef KSTACK_USAGE_TRACK
void proc_kstack_paint(void);
uint16_t proc_kstack_scan(void);
uint16_t proc_kstack_capacity(void);
void proc_kstack_usage_report(void);
#endif

/* Re-plant every slot's canary.  Called from sched_start() on arches
 * where the boot stack may have overwritten canaries planted in
 * proc_init() (ia16). */
void proc_plant_kstack_canaries(void);

/* Verify every slot's canary.  Called from end-of-syscall and
 * end-of-vfork-restore paths so an overrun is caught at the next
 * kernel→user transition. */
void proc_check_kstack_canary_panic(void);

#endif /* PPAP_KERNEL_CORE_PROC_KSTACK_H */
