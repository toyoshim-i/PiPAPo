/*
 * cpu_native.c — cpu_ops_t for the host's native CPU
 *
 * Provides a cpu_ops_t implementation for running native binaries on
 * the host architecture.  Memory access functions are direct pointer
 * operations.  The run() function is a stub — native processes use the
 * scheduler's context switch mechanism (PendSV on ARM, RTE on m68k)
 * rather than an interpreter loop.  This will be connected in Phase 2
 * when do_execve is refactored.
 */

#include <string.h>

#include "kernel/cpu/cpu.h"
#include "kernel/mm/page.h"

/* ── Native CPU state ──────────────────────────────────────────────────── */

typedef struct native_cpu_state {
  uint8_t *memory;
  uint32_t mem_size;
} native_cpu_state_t;

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/*
 * Native CPU state is only used transiently during ELF loading (GOT/reloc
 * fixup via read32/write32 passthrough to physical memory).  A single
 * static instance avoids wasting a 4 KB page per native process.
 */
static native_cpu_state_t native_state;

static void *native_create_state(void) {
  memset(&native_state, 0, sizeof(native_state));
  return &native_state;
}

static int native_init(void *state, uint8_t *memory, uint32_t mem_size) {
  native_cpu_state_t *s = (native_cpu_state_t *)state;
  s->memory = memory;
  s->mem_size = mem_size;
  return 0;
}

/* ── Run (stub) ────────────────────────────────────────────────────────── *
 *
 * Native processes do not use an interpreter loop.  They return to user
 * mode via the scheduler's exception return (PendSV on ARM, RTE on m68k).
 * This function should not be called in Phase 1; it will be properly
 * implemented in Phase 2.
 */
static int native_run(void *state) {
  (void)state;
  return -1; /* should not be called yet */
}

/* ── Trap handler (no-op for native) ───────────────────────────────────── */

static void native_set_trap_handler(void *state, cpu_trap_handler_t handler,
                                    void *ctx) {
  (void)state;
  (void)handler;
  (void)ctx;
}

/* ── Register access (not meaningful for native in Phase 1) ────────────── */

static uint32_t native_get_reg(void *state, int reg_id) {
  (void)state;
  (void)reg_id;
  return 0;
}

static void native_set_reg(void *state, int reg_id, uint32_t val) {
  (void)state;
  (void)reg_id;
  (void)val;
}

/* ── Memory access (direct, endian-aware) ──────────────────────────────── */

static void *native_translate_ptr(void *state, uint32_t guest_addr,
                                  uint32_t size) {
  native_cpu_state_t *s = (native_cpu_state_t *)state;
  if (guest_addr + size > s->mem_size) return NULL;
  return s->memory + guest_addr;
}

static uint8_t native_read8(void *state, uint32_t addr) {
  native_cpu_state_t *s = (native_cpu_state_t *)state;
  if (addr >= s->mem_size) return 0;
  return s->memory[addr];
}

static void native_write8(void *state, uint32_t addr, uint8_t val) {
  native_cpu_state_t *s = (native_cpu_state_t *)state;
  if (addr >= s->mem_size) return;
  s->memory[addr] = val;
}

/*
 * 16-bit access uses the host's native byte order:
 *   ARM (little-endian): low byte first
 *   m68k (big-endian):   high byte first
 */
static uint16_t native_read16(void *state, uint32_t addr) {
  native_cpu_state_t *s = (native_cpu_state_t *)state;
  if (addr + 1 >= s->mem_size) return 0;
#if defined(__m68k__)
  /* Big-endian */
  return ((uint16_t)s->memory[addr] << 8) | s->memory[addr + 1];
#else
  /* Little-endian (ARM) */
  return s->memory[addr] | ((uint16_t)s->memory[addr + 1] << 8);
#endif
}

static void native_write16(void *state, uint32_t addr, uint16_t val) {
  native_cpu_state_t *s = (native_cpu_state_t *)state;
  if (addr + 1 >= s->mem_size) return;
#if defined(__m68k__)
  /* Big-endian */
  s->memory[addr] = (val >> 8) & 0xFF;
  s->memory[addr + 1] = val & 0xFF;
#else
  /* Little-endian (ARM) */
  s->memory[addr] = val & 0xFF;
  s->memory[addr + 1] = (val >> 8) & 0xFF;
#endif
}

static uint32_t native_read32(void *state, uint32_t addr) {
  native_cpu_state_t *s = (native_cpu_state_t *)state;
  if (addr + 3 >= s->mem_size) return 0;
#if defined(__m68k__)
  /* Big-endian */
  return ((uint32_t)s->memory[addr] << 24) |
         ((uint32_t)s->memory[addr + 1] << 16) |
         ((uint32_t)s->memory[addr + 2] << 8) | s->memory[addr + 3];
#else
  /* Little-endian (ARM) */
  return s->memory[addr] | ((uint32_t)s->memory[addr + 1] << 8) |
         ((uint32_t)s->memory[addr + 2] << 16) |
         ((uint32_t)s->memory[addr + 3] << 24);
#endif
}

static void native_write32(void *state, uint32_t addr, uint32_t val) {
  native_cpu_state_t *s = (native_cpu_state_t *)state;
  if (addr + 3 >= s->mem_size) return;
#if defined(__m68k__)
  /* Big-endian */
  s->memory[addr] = (val >> 24) & 0xFF;
  s->memory[addr + 1] = (val >> 16) & 0xFF;
  s->memory[addr + 2] = (val >> 8) & 0xFF;
  s->memory[addr + 3] = val & 0xFF;
#else
  /* Little-endian (ARM) */
  s->memory[addr] = val & 0xFF;
  s->memory[addr + 1] = (val >> 8) & 0xFF;
  s->memory[addr + 2] = (val >> 16) & 0xFF;
  s->memory[addr + 3] = (val >> 24) & 0xFF;
#endif
}

/* ── cpu_ops_t table ───────────────────────────────────────────────────── */

const cpu_ops_t native_cpu_ops = {
#if defined(__m68k__)
    .name = "native-m68k",
    .arch_id = CPU_ARCH_M68K,
#elif defined(__riscv)
    .name = "native-riscv",
    .arch_id = CPU_ARCH_RISCV,
#elif defined(__xtensa__)
    .name = "native-xtensa",
    .arch_id = CPU_ARCH_XTENSA,
#elif defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
    .name = "native-arm_m",
    .arch_id = CPU_ARCH_ARM,
#elif defined(__ia16__)
    .name = "native-i16",
    .arch_id = CPU_ARCH_8086,
#else
#error "Unknown architecture — add native_cpu_ops entry"
#endif
    .create_state = native_create_state,
    .init = native_init,
    .run = native_run,
    .set_trap_handler = native_set_trap_handler,
    .get_reg = native_get_reg,
    .set_reg = native_set_reg,
    .translate_ptr = native_translate_ptr,
    .read8 = native_read8,
    .write8 = native_write8,
    .read16 = native_read16,
    .write16 = native_write16,
    .read32 = native_read32,
    .write32 = native_write32,
};
