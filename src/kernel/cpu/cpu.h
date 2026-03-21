/*
 * cpu.h — Common CPU interface
 *
 * This provides a common interface for both native and emulated CPUs so the
 * kernel and subsystem loader layers can interact with any architecture
 * uniformly.
 */

#ifndef PPAP_KERNEL_CPU_CPU_H
#define PPAP_KERNEL_CPU_CPU_H

#include <stdint.h>

// Forward declaration of pcb_t
typedef struct pcb pcb_t;

/* ── Opaque CPU state ────────────────────────────────────────────────── */
typedef struct cpu_state cpu_state_t;

/* ── Trap types (common across all cores) ────────────────────────────── */
#define CPU_TRAP_RST 0     /* RST instruction (Z80 restart vector)     */
#define CPU_TRAP_SWI 1     /* Software interrupt / trap instruction    */
#define CPU_TRAP_HALT 2    /* CPU halt instruction                     */
#define CPU_TRAP_IO_IN 3   /* I/O port read                            */
#define CPU_TRAP_IO_OUT 4  /* I/O port write                           */
#define CPU_TRAP_ILLEGAL 5 /* Illegal/undefined instruction            */

/* ── Return values from trap handler ─────────────────────────────────── */
#define CPU_TRAP_UNHANDLED 0 /* Core should execute normally           */
#define CPU_TRAP_HANDLED 1   /* Personality handled it, continue       */
#define CPU_TRAP_EXIT 2      /* Process should exit                    */

/* ── Architecture IDs ────────────────────────────────────────────────── */
#define CPU_ARCH_Z80 1
#define CPU_ARCH_M68K 2
#define CPU_ARCH_8086 3
#define CPU_ARCH_ARM 4
#define CPU_ARCH_ARMV6 5

/* Host architecture — set by compiler target.
 * Host-only test builds define PPAP_HOST_TEST to skip this check. */
#if defined(__m68k__)
#define HOST_ARCH_ID CPU_ARCH_M68K
#elif defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
#define HOST_ARCH_ID CPU_ARCH_ARM
#elif !defined(PPAP_HOST_TEST)
#error "Unsupported host architecture"
#endif

/* ── Trap handler callback ───────────────────────────────────────────── */
typedef int (*cpu_trap_handler_t)(cpu_state_t* cpu, int trap_type,
                                  uint32_t param, void* ctx);

/* ── Core operations table ───────────────────────────────────────────── */
typedef struct cpu_ops {
  const char* name;
  int arch_id;

  // Allocate and initialize a CPU state structure.
  // For native CPUs, this might be minimal. For eCPUs, this allocates the
  // emulator state.
  void* (*create_state)(void);

  // Initialize the CPU for a new process. `memory` is a pointer to the
  // process's address space.
  int (*init)(void* state, uint8_t* memory, uint32_t mem_size);

  // The entry point for running the process's code.
  // For eCPUs, this is the emulator loop.
  // For native CPUs, this function sets up the hardware context and jumps to
  // user code.
  int (*run)(void* state);

  // Set a trap/exception handler.
  void (*set_trap_handler)(void* state, cpu_trap_handler_t handler, void* ctx);

  // Register access
  uint32_t (*get_reg)(void* state, int reg_id);
  void (*set_reg)(void* state, int reg_id, uint32_t val);

  // Memory access
  void* (*translate_ptr)(void* state, uint32_t guest_addr, uint32_t size);
  uint8_t (*read8)(void* state, uint32_t addr);
  void (*write8)(void* state, uint32_t addr, uint8_t val);
  uint16_t (*read16)(void* state, uint32_t addr);
  void (*write16)(void* state, uint32_t addr, uint16_t val);
  uint32_t (*read32)(void* state, uint32_t addr);
  void (*write32)(void* state, uint32_t addr, uint32_t val);

} cpu_ops_t;

/* ── CPU ops registry ───────────────────────────────────────────────── */

/* Find a cpu_ops_t by architecture ID. Returns NULL if not found. */
const cpu_ops_t* cpu_ops_for_arch(int arch_id);

/* Log native and emulated CPUs during boot. */
void cpu_init(void);

/* ── Available CPU ops (defined in respective .c files) ─────────────── */

/* Native CPU ops (only available for the host architecture) */
extern const cpu_ops_t native_cpu_ops;

/* eCPU ops — declared in their respective headers:
 *   ecpu_z80_ops  in kernel/cpu/ecpu_z80.h
 *   ecpu_m68k_ops in kernel/cpu/ecpu_m68k.h
 */

#endif /* PPAP_KERNEL_CPU_CPU_H */
