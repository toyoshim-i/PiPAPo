/*
 * ecpu.h — Common eCPU emulator interface
 *
 * All eCPU emulator cores (Z80, m68k, 8086, ARM, etc.) implement this
 * common interface so the kernel and subsystem personality layers can
 * interact with any emulated architecture uniformly.
 *
 * See docs/ecpu/z80.md §3 for the full design rationale.
 */

#ifndef PPAP_ECPU_ECPU_H
#define PPAP_ECPU_ECPU_H

#include <stdint.h>

/* ── Opaque emulator state ───────────────────────────────────────────────────
 * Each core defines its own concrete struct (e.g. z80_state_t).
 * Personality layers and kernel code use this opaque typedef. */
typedef struct ecpu_state ecpu_state_t;

/* ── Trap types (common across all cores) ────────────────────────────────── */
#define ECPU_TRAP_RST      0   /* RST instruction (Z80 restart vector)     */
#define ECPU_TRAP_SWI      1   /* Software interrupt / trap instruction    */
#define ECPU_TRAP_HALT     2   /* CPU halt instruction                     */
#define ECPU_TRAP_IO_IN    3   /* I/O port read                            */
#define ECPU_TRAP_IO_OUT   4   /* I/O port write                           */
#define ECPU_TRAP_ILLEGAL  5   /* Illegal/undefined instruction            */

/* ── Return values from trap handler ─────────────────────────────────────── */
#define ECPU_TRAP_UNHANDLED  0   /* Core should execute normally           */
#define ECPU_TRAP_HANDLED    1   /* Personality handled it, continue       */
#define ECPU_TRAP_EXIT       2   /* Process should exit                    */

/* ── Architecture IDs ────────────────────────────────────────────────────── */
#define ECPU_ARCH_Z80     1
#define ECPU_ARCH_M68K    2
#define ECPU_ARCH_8086    3
#define ECPU_ARCH_ARM     4
#define ECPU_ARCH_ARMV6   5
#define ECPU_ARCH_X86     6

/* ── Trap handler callback ───────────────────────────────────────────────── */
typedef int (*ecpu_trap_handler_t)(ecpu_state_t *cpu, int trap_type,
                                   uint32_t param, void *ctx);

/* ── Core operations table ───────────────────────────────────────────────── */
typedef struct ecpu_core_ops {
    const char *name;              /* "z80", "m68k", "8086", etc.          */
    uint32_t    arch_id;           /* ECPU_ARCH_xxx                        */

    /* Lifecycle */
    int  (*init)(ecpu_state_t *cpu, uint8_t *memory, uint32_t mem_size);
    void (*reset)(ecpu_state_t *cpu);
    int  (*run)(ecpu_state_t *cpu);  /* enter interpreter loop             */
    int  (*step)(ecpu_state_t *cpu); /* execute one instruction            */

    /* Trap hook registration */
    void (*set_trap_handler)(ecpu_state_t *cpu,
                             ecpu_trap_handler_t handler, void *ctx);

    /* Register access by register ID (core-specific IDs) */
    uint32_t (*get_reg)(ecpu_state_t *cpu, int reg_id);
    void     (*set_reg)(ecpu_state_t *cpu, int reg_id, uint32_t val);

    /* Memory access */
    void    *(*translate_ptr)(ecpu_state_t *cpu, uint32_t guest_addr,
                              uint32_t size);
    uint8_t  (*read8)(ecpu_state_t *cpu, uint32_t addr);
    void     (*write8)(ecpu_state_t *cpu, uint32_t addr, uint8_t val);
    uint16_t (*read16)(ecpu_state_t *cpu, uint32_t addr);
    void     (*write16)(ecpu_state_t *cpu, uint32_t addr, uint16_t val);

    /* State size (for allocation) */
    uint32_t state_size;
} ecpu_core_ops_t;

#endif /* PPAP_ECPU_ECPU_H */
