/*
 * core_mod_init.c — VFS-side mod_core struct (i16 segment split)
 *
 * On i16, the mod_core struct lives in the VFS module and points
 * to the caller-side stubs (core_stubs.S). The stubs do the far
 * call to the core segment.
 *
 * Linked into ppap_ibmpc_vfs (VFS binary).
 */

#ifdef __ia16__

#include "mm/kmem.h"  /* kmem_pool_t */
#include "common/mod/module.h"

/* Forward-declare the caller-side stubs from core_stubs.S.
 * Stubs use unprefixed names (klog, kmem_alloc, etc.).
 * MOD_IMPL(core, klog) expands to .klog = core_klog, so we
 * alias the prefixed names to the unprefixed stubs. */
void klog(const char *);
void klogf(const char *, ...);
void kmem_pool_init(kmem_pool_t *, void *, size_t, uint32_t);
void *kmem_alloc(kmem_pool_t *);
void kmem_free(kmem_pool_t *, void *);
uint32_t kmem_free_count(const kmem_pool_t *);

#define core_klog           klog
#define core_klogf          klogf
#define core_kmem_pool_init kmem_pool_init
#define core_kmem_alloc     kmem_alloc
#define core_kmem_free      kmem_free
#define core_kmem_free_count kmem_free_count

#include "common/mod/mod_core.h"

MOD_DEFINE_BEGIN(core)
  MOD_IMPL(core, klog)
  MOD_IMPL(core, klogf)
  MOD_IMPL(core, kmem_pool_init)
  MOD_IMPL(core, kmem_alloc)
  MOD_IMPL(core, kmem_free)
  MOD_IMPL(core, kmem_free_count)
MOD_DEFINE_END()

#endif /* __ia16__ */
