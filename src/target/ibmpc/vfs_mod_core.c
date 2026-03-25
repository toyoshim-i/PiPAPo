/*
 * vfs_mod_core.c — mod_core struct for the VFS binary
 *
 * In the VFS separate binary, mod_core functions are asm stubs
 * that do lcall to the core segment. This file wires the struct.
 */

#include "kernel/common/mod/mod_core.h"
#include "kernel/klog.h"
#include "kernel/mm/kmem.h"

/* The asm stubs in vfs_core_stubs.S have the same names as the
 * real functions (klog, klogf, kmem_alloc, etc.). The mod_core
 * struct points to them. */
mod_core_t mod_core = {
  .klog = klog,
  .klogf = klogf,
  .kmem_pool_init = kmem_pool_init,
  .kmem_alloc = kmem_alloc,
  .kmem_free = kmem_free,
  .kmem_free_count = kmem_free_count,
};
