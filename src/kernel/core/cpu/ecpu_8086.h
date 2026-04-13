/*
 * ecpu_8086.h — i8086 emulator core state and register IDs
 */

#ifndef PPAP_KERNEL_CORE_CPU_ECPU_8086_H
#define PPAP_KERNEL_CORE_CPU_ECPU_8086_H

#include "kernel/core/cpu/cpu.h"

/* ── 8086 register IDs (for get_reg/set_reg) ───────────────────────────── */
#define CPU_REG_AX 0
#define CPU_REG_CX 1
#define CPU_REG_DX 2
#define CPU_REG_BX 3
#define CPU_REG_SP 4
#define CPU_REG_BP 5
#define CPU_REG_SI 6
#define CPU_REG_DI 7
#define CPU_REG_ES 8
#define CPU_REG_CS 9
#define CPU_REG_SS 10
#define CPU_REG_DS 11
#define CPU_REG_IP 12
#define CPU_REG_FLAGS 13

#endif /* PPAP_KERNEL_CORE_CPU_ECPU_8086_H */
