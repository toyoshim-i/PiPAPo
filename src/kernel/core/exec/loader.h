/*
 * loader.h — Common binary format loader interface
 */

#ifndef PPAP_KERNEL_CORE_EXEC_LOADER_H
#define PPAP_KERNEL_CORE_EXEC_LOADER_H

#include <stdint.h>

#include "kernel/core/cpu/cpu.h"

// Forward declaration of pcb_t and vnode_t
typedef struct pcb pcb_t;
typedef struct vnode vnode_t;

/* Flags passed to loader_t.load() */
#define EXEC_FLAG_XIP_SOURCE                      \
  (1u << 0) /* file buffer is XIP-capable (romfs) \
             */

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

  // Load the binary.
  // This function is responsible for populating memory and setting the initial
  // CPU state (PC, SP, etc.) via the provided cpu_ops interface. It should not
  // modify the pcb_t directly, other than what's needed for loading.
  // flags: EXEC_FLAG_* bits (e.g. EXEC_FLAG_XIP_SOURCE).
  //
  // Legacy entry point that receives a staging buffer dereferenceable as a
  // flat pointer.  Broken on i16 when the staging buffer sits above the
  // first 64 KB; loaders migrate to load_vn (below) one per commit, and
  // this field is retired once every registered loader sets load_vn.
  int (*load)(pcb_t* p, const uint8_t* file_buf, uint32_t file_size,
              const cpu_ops_t* cpu_ops, void* cpu_state,
              const char* const* argv, uint32_t flags);

  // Load the binary by streaming from `vn` via mod_vfs.vnode_read.
  // If non-NULL, exec.c calls this instead of `load` and skips the staging
  // buffer allocation.  Preferred for all new loader implementations.
  int (*load_vn)(pcb_t* p, vnode_t* vn, uint32_t file_size,
                 const cpu_ops_t* cpu_ops, void* cpu_state,
                 const char* const* argv, uint32_t flags);

  // The required CPU architecture for this loader.
  int required_arch_id;

  // If true, the loader executes code directly from the file buffer (XIP).
  // The coordinator must keep the buffer alive after a successful load.
  // If false, the coordinator frees the file buffer after loading.
  // Only consulted for `load`; `load_vn` manages its own source lifetime.
  int xip;

} loader_t;

#endif /* PPAP_KERNEL_CORE_EXEC_LOADER_H */
