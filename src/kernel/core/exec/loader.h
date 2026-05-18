/*
 * loader.h — Common binary format loader interface
 */

#ifndef PPAP_KERNEL_CORE_EXEC_LOADER_H
#define PPAP_KERNEL_CORE_EXEC_LOADER_H

#include <stdint.h>

#include "kernel/core/cpu/cpu.h"
#include "kernel/core/exec/image_alloc.h"

// Forward declaration of pcb_t and vnode_t
typedef struct pcb pcb_t;
typedef struct vnode vnode_t;
typedef struct exec_args exec_args_t;

/* Flag passed to loader internals that want to know whether their source
 * is XIP-capable (e.g. elf_load, which may execute text in place from
 * romfs flash).  Not interpreted by exec.c anymore — loaders that care
 * set it themselves when calling their internal helpers. */
#define EXEC_FLAG_XIP_SOURCE (1u << 0)

/* Maximum number of bytes exec.c pre-reads from the file start and hands to
 * each loader's detect().  Large enough to cover every current magic check
 * (ELF32 ehdr = 52 bytes, X68K header = 64 bytes). */
#define LOADER_HEADER_MAX 64u

typedef struct loader {
  const char* name;

  // Detect if the loader can handle this file.
  // `header` points to the first min(file_size, LOADER_HEADER_MAX) bytes of
  // the file (lives on the caller's kernel stack).  `header_len` is how many
  // bytes are valid.  Loaders that match on extension ignore `header`.
  int (*detect)(const uint8_t* header, uint32_t header_len, uint32_t file_size,
                const char* path);

  // Load the binary by streaming from `vn` via mod_vfs.vnode_read.
  // Responsible for populating memory and setting the initial CPU state
  // (PC, SP, etc.) via the provided cpu_ops interface.  Should not modify
  // pcb_t beyond what's needed for loading.
  //
  // `args` carries the (path, argv[], envp[]) triple captured by
  // sys_execve in a data-region page.  Loaders read individual entries
  // through the exec_args_* accessors (see exec_args.h).  argv may be
  // empty (args->argc == 0) and envp may be empty (args->envc == 0); a
  // loader may ignore envp when its target personality has no env
  // concept (CP/M, SOS).
  int (*load)(pcb_t* p, vnode_t* vn, uint32_t file_size,
              const cpu_ops_t* cpu_ops, void* cpu_state,
              const exec_args_t* args, uint32_t flags);

  // The required CPU architecture for this loader.
  int required_arch_id;

} loader_t;

#endif /* PPAP_KERNEL_CORE_EXEC_LOADER_H */
