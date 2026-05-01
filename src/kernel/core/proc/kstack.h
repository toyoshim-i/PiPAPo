/*
 * kstack.h — per-process kernel-stack init + overflow sentinels.
 *
 * proc_init / proc_alloc call proc_kstack_init_slot() to set up the
 * per-process kernel stack pointer (PCB.kernel_sp) for a given slot,
 * and proc_kstack_plant_canary() to plant the slot's overflow
 * sentinel.  proc_check_kstack_canary_panic() walks every slot's
 * sentinel from end-of-syscall paths; on mismatch it logs and halts.
 *
 * Implementation in kstack.c is selected at compile time by
 * PROC_HAS_FIXED_REGION_KSTACK (config.h, set per arch + per-target
 * opt-in flag): fixed-region for targets that own a linker-reserved
 * __kstack_region_base, no-op otherwise.
 */

#ifndef PPAP_KERNEL_CORE_PROC_KSTACK_H
#define PPAP_KERNEL_CORE_PROC_KSTACK_H

#include <stdint.h>

#include "kernel/common/core/proc_info.h"

/* Linker-provided base of the per-process kernel-stack region.
 * Defined by target linker scripts (e.g. pcxt_kernel.ld, qemu.ld) on
 * targets whose per-arch overlay implements the fixed-region scheme.
 * Declared here so the overlays do not need an `extern` in their .c. */
extern char __kstack_region_base[];

void proc_kstack_init_slot(pcb_t *p, uint32_t slot_idx);
void proc_kstack_plant_canary(uint32_t slot_idx);

/* Re-plant every slot's canary.  Called from sched_start() on arches
 * where the boot stack may have overwritten canaries planted in
 * proc_init() (ia16). */
void proc_plant_kstack_canaries(void);

/* Verify every slot's canary.  Called from end-of-syscall and
 * end-of-vfork-restore paths so an overrun is caught at the next
 * kernel→user transition. */
void proc_check_kstack_canary_panic(void);

#endif /* PPAP_KERNEL_CORE_PROC_KSTACK_H */
