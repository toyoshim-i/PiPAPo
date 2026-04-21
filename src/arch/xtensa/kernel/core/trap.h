/*
 * trap.h -- Xtensa exception / syscall trap dispatch API
 *
 * xtensa_trap_init() installs PPAP's syscall + exception handlers into
 * the Xtensa dispatcher and sets xtensa_trap_ready so callers that
 * gate on it can observe completion.  Targets call it during
 * late_init after xtensa_timer_init().
 */

#ifndef PPAP_ARCH_XTENSA_KERNEL_CORE_TRAP_H
#define PPAP_ARCH_XTENSA_KERNEL_CORE_TRAP_H

#include <stdint.h>

void xtensa_trap_init(void);

/* 0 until xtensa_trap_init() returns, 1 afterwards.  Used by PPAP
 * bootstrap-boundary checks that assert the trap path is owned by PPAP
 * rather than still sitting on the ESP-IDF / FreeRTOS default. */
extern volatile uint32_t xtensa_trap_ready;

#endif /* PPAP_ARCH_XTENSA_KERNEL_CORE_TRAP_H */
