/*
 * mpu.h — MPU 4-region layout (Cortex-M0+, RP2040)
 *
 * Configures four MPU regions at boot time and updates region 2 on each
 * context switch so that only the current process's stack page is accessible
 * from unprivileged (user) mode.
 *
 * QEMU note: mps2-an500 does not emulate the RP2040 MPU.  mpu_init() reads
 * MPU_TYPE; if it returns 0 (no MPU), the function returns immediately.
 * mpu_switch() is a no-op in that case.  Neither function needs a separate
 * #ifdef for QEMU — the hardware check handles both targets transparently.
 */

#ifndef PPAP_MM_MPU_H
#define PPAP_MM_MPU_H

/* Forward declaration — full definition in proc/proc.h */
typedef struct pcb pcb_t;

/*
 * mpu_init() — program static MPU regions 0, 1, 3 and enable the MPU.
 *
 * Regions:
 *   0  0x20000000  16 KB  kernel data    — RW privileged only, XN
 *   1  0x10000000  16 MB  flash (XIP)    — RO all modes, executable
 *   2  (skipped)          process stack  — set by mpu_switch()
 *   3  0x40000000 512 MB  peripherals    — RW privileged only, XN
 *
 * MPU_CTRL.PRIVDEFENA is set so that privileged code retains full access
 * to all memory via the default background map; the MPU only restricts
 * unprivileged accesses.
 *
 * Must be called once from kmain() before sched_start().
 */
void mpu_init(void);

/*
 * mpu_switch(next) — reprogram MPU region 2 for the incoming process.
 *
 * Called from PendSV_Handler (switch.S) on every context switch.
 * If next->stack_page is NULL, region 2 is disabled.
 * No-op when no MPU is present (mpu_init() detected QEMU / no MPU).
 */
void mpu_switch(pcb_t *next);

#endif /* PPAP_MM_MPU_H */
