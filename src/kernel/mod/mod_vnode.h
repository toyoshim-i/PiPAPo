/*
 * mod_vnode.h — Vnode pool kernel module interface
 *
 * Defines the module boundary for vnode lifecycle management.
 * Separated from mod_vfs because the callers are different:
 *
 *   mod_vfs  — called by syscalls, exec, main.c (path operations)
 *   mod_vnode — called by every filesystem driver (romfs, tmpfs,
 *               devfs, procfs, ufs, vfat) and by syscall cleanup
 *
 * The vnode pool is a fixed-size slab managed by the kmem allocator.
 * Each vnode represents an in-memory file/directory/device node.
 * Vnodes are reference-counted; when the count drops to zero, the
 * vnode is returned to the pool.
 *
 * Callers:
 *   32-bit: mod_vnode.alloc(), mod_vnode.put(vn), etc.
 *   i16:    vnode_alloc(), vnode_put(vn), etc.
 *
 * Implementation: src/kernel/vfs/vfs.c (pool management)
 */

#ifndef PPAP_KERNEL_MOD_MOD_VNODE_H
#define PPAP_KERNEL_MOD_MOD_VNODE_H

#include "module.h"

struct vnode;
typedef struct vnode vnode_t;

MOD_DECLARE_BEGIN(vnode)

  /*
   * alloc — Allocate a vnode from the pool.
   *
   * Returns a zeroed vnode with refcount 1, or NULL if the pool
   * is exhausted.  The caller must set vn->ops, vn->type, etc.
   * before making the vnode visible to other subsystems.
   */
  MOD_FUNC(vnode, vnode_t *, alloc, void)

  /*
   * ref — Increment a vnode's reference count.
   *
   *   vn  Vnode to reference.
   *
   * Called when a new file descriptor or directory entry points to
   * an existing vnode (e.g. dup, fork fd inheritance, hardlink).
   */
  MOD_FUNC(vnode, void, ref, vnode_t *)

  /*
   * put — Decrement a vnode's reference count and free if zero.
   *
   *   vn  Vnode to release.
   *
   * When the refcount reaches zero, the vnode is returned to the
   * pool.  Safe to call with NULL (no-op).
   *
   * Called by sys_close, do_execve cleanup, process exit, and
   * any code that obtained a vnode via vfs_lookup.
   */
  MOD_FUNC(vnode, void, put, vnode_t *)

MOD_DECLARE_END(vnode)

#endif /* PPAP_KERNEL_MOD_MOD_VNODE_H */
