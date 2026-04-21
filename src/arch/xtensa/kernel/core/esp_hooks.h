/*
 * esp_hooks.h -- ESP-IDF / FreeRTOS internal symbols PPAP relies on
 *
 * These symbols are defined inside ESP-IDF (xtensa_intr_asm.S) or
 * FreeRTOS (port.c) but not exported via any public ESP-IDF header.
 * Declare only the subset PPAP needs so xtensa_common.c doesn't have
 * to carry raw externs in its body.
 */

#ifndef PPAP_ARCH_XTENSA_KERNEL_CORE_ESP_HOOKS_H
#define PPAP_ARCH_XTENSA_KERNEL_CORE_ESP_HOOKS_H

#include <stdint.h>

#include "xtensa_api.h" /* xt_exc_handler typedef */

/* Xtensa dispatcher table (xtensa_intr_asm.S).  Indexed by EXCCAUSE;
 * each slot holds the handler the ESP-IDF dispatcher jumps to.  PPAP
 * writes directly into this table to override the defaults. */
extern xt_exc_handler _xt_exception_table[];

/* FreeRTOS per-core "scheduler running" flag.  _frxt_int_enter /
 * _frxt_int_exit consult it; clearing it disables FreeRTOS's interrupt-
 * level context switching so PPAP's scheduler owns all task switching. */
extern uint32_t port_xSchedulerRunning[];

#endif /* PPAP_ARCH_XTENSA_KERNEL_CORE_ESP_HOOKS_H */
