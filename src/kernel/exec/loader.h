/*
 * loader.h — Common binary format loader interface
 */

#ifndef PPAP_KERNEL_LOADER_H
#define PPAP_KERNEL_LOADER_H

#include <stdint.h>
#include "kernel/cpu/cpu.h"

// Forward declaration of pcb_t
typedef struct pcb pcb_t;

typedef struct loader {
    const char* name;

    // Detect if the loader can handle this file.
    int (*detect)(const uint8_t* file_buf, uint32_t file_size, const char* path);

    // Load the binary.
    // This function is responsible for populating memory and setting the initial CPU state
    // (PC, SP, etc.) via the provided cpu_ops interface.
    // It should not modify the pcb_t directly, other than what's needed for loading.
    int (*load)(pcb_t* p, const uint8_t* file_buf, uint32_t file_size,
                const cpu_ops_t* cpu_ops, void* cpu_state,
                const char* const* argv);

    // The required CPU architecture for this loader.
    int required_arch_id;

} loader_t;

#endif /* PPAP_KERNEL_LOADER_H */
