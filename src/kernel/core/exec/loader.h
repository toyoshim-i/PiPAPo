/*
 * loader.h — Common binary format loader interface
 */

#ifndef PPAP_KERNEL_EXEC_LOADER_H
#define PPAP_KERNEL_EXEC_LOADER_H

#include <stdint.h>

#include "kernel/core/cpu/cpu.h"

// Forward declaration of pcb_t
typedef struct pcb pcb_t;

/* Flags passed to loader_t.load() */
#define EXEC_FLAG_XIP_SOURCE (1u << 0) /* file buffer is XIP-capable (romfs) */

typedef struct loader {
  const char* name;

  // Detect if the loader can handle this file.
  int (*detect)(const uint8_t* file_buf, uint32_t file_size, const char* path);

  // Load the binary.
  // This function is responsible for populating memory and setting the initial
  // CPU state (PC, SP, etc.) via the provided cpu_ops interface. It should not
  // modify the pcb_t directly, other than what's needed for loading.
  // flags: EXEC_FLAG_* bits (e.g. EXEC_FLAG_XIP_SOURCE).
  int (*load)(pcb_t* p, const uint8_t* file_buf, uint32_t file_size,
              const cpu_ops_t* cpu_ops, void* cpu_state,
              const char* const* argv, uint32_t flags);

  // The required CPU architecture for this loader.
  int required_arch_id;

  // If true, the loader executes code directly from the file buffer (XIP).
  // The coordinator must keep the buffer alive after a successful load.
  // If false, the coordinator frees the file buffer after loading.
  int xip;

} loader_t;

#endif /* PPAP_KERNEL_EXEC_LOADER_H */
