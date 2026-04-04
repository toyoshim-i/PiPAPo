/*
 * cpu.c — CPU operations registry
 *
 * Maintains a list of all available cpu_ops_t implementations (native
 * and emulated) and provides a lookup function by architecture ID.
 */

#include "kernel/core/cpu/cpu.h"

#include <stddef.h>

#include "kernel/common/mod/mod_vfs.h"

#ifdef PPAP_ENABLE_ECPU_Z80
#include "kernel/core/cpu/ecpu_z80.h"
#endif
#ifdef PPAP_ENABLE_ECPU_M68K
#include "kernel/core/cpu/ecpu_m68k.h"
#endif

/* ── Registry of available CPU ops ─────────────────────────────────────── */

static const cpu_ops_t *cpu_ops_registry[] = {&native_cpu_ops,
#ifdef PPAP_ENABLE_ECPU_Z80
                                              &ecpu_z80_ops,
#endif
#ifdef PPAP_ENABLE_ECPU_M68K
                                              &ecpu_m68k_ops,
#endif
                                              NULL};

/* ── Lookup by arch ID ─────────────────────────────────────────────────── */

const cpu_ops_t *cpu_ops_for_arch(int arch_id) {
  for (int i = 0; cpu_ops_registry[i] != NULL; i++) {
    if (cpu_ops_registry[i]->arch_id == arch_id) return cpu_ops_registry[i];
  }
  return NULL;
}

/* ── Boot log ──────────────────────────────────────────────────────────── */

void cpu_init(void) {
  mod_vfs.klogf("CPU: %s", native_cpu_ops.name);

  int first = 1;
  for (int i = 0; cpu_ops_registry[i] != NULL; i++) {
    if (cpu_ops_registry[i] == &native_cpu_ops) continue;
    mod_vfs.klogf("%s %s", first ? " + emulated" : ",", cpu_ops_registry[i]->name);
    first = 0;
  }
  mod_vfs.klogf("\n");
}
