/*
 * exec.c — execve coordinator for PPAP
 *
 * Reads the executable file from the VFS, iterates the loader registry
 * to find a matching binary format, and delegates loading to the
 * matched loader.  Post-load, sets process metadata (comm, cwd,
 * signals) and manages the file buffer lifecycle.
 */

#include "kernel/core/exec/exec.h"

#include <string.h>

#include "common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/arch.h"
#include "kernel/core/exec/elf.h"
#include "kernel/core/exec/loader.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/signal/signal.h"

#if defined(__ia16__)
/* elf16 is kept on its own streaming entrypoint until Phase 3.2 folds it
 * into the common vnode-based loader contract (see
 * docs/proposals/loader_revamp.md). */
extern const loader_t elf16_loader;
int elf16_load_vnode(pcb_t *p, vnode_t *vn, uint32_t file_size,
                     const cpu_ops_t *cpu_ops, void *cpu_state,
                     const char *const *argv, uint32_t flags);
#endif

/* Read `len` bytes from the start of `vn` into `buf`.  Converts `buf` to a
 * (page_id, page_off) pair so mod_vfs.vnode_read does not dereference `buf`
 * on i16 where a 32-bit linear address would truncate to a near pointer.
 */
static long exec_read_from(vnode_t *vn, void *buf, uint32_t len) {
  uintptr_t addr = (uintptr_t)buf;
  page_id_t page = (page_id_t)(addr / PAGE_SIZE);
  uint16_t page_off = (uint16_t)(addr & (PAGE_SIZE - 1u));
  return mod_vfs.vnode_read(vn, page, page_off, len, 0);
}

/* ── execve ─────────────────────────────────────────────────────────── */

int exec_execve(pcb_t *p, const char *path, const char *const *argv) {
  vnode_t *vn = NULL;
  int err;

  const char *default_argv[2] = {path, NULL};
  if (!argv || !argv[0]) {
    argv = default_argv;
  }

  /* ── 1. Look up the binary in the VFS ──────────────────────────────── */
  err = mod_vfs.lookup(path, &vn);
  if (err < 0) return err;

  if (vn->type != VNODE_FILE) {
    mod_vfs.vnode_release(vn);
    return -(int)ENOEXEC;
  }

  /* ── 2. Read a header buffer for loader detect() ───────────────────── */
  uint32_t file_size = vn->size;
  extern const loader_t *loader_registry[];
  int rc = -(int)ENOEXEC;

  if (file_size == 0) {
    mod_vfs.vnode_release(vn);
    return -(int)ENOEXEC;
  }

  uint8_t header[LOADER_HEADER_MAX];
  uint32_t header_len =
      file_size < LOADER_HEADER_MAX ? file_size : LOADER_HEADER_MAX;
  long nread = exec_read_from(vn, header, header_len);
  if (nread < 0 || (uint32_t)nread != header_len) {
    mod_vfs.vnode_release(vn);
    return (nread < 0) ? (int)nread : -(int)ENOEXEC;
  }

#if defined(__ia16__)
  /* On i16 the legacy staging-buffer path below truncates to a near
   * pointer (Phase 3 retargets loaders to stream via vnode_read).  Until
   * then, elf16 uses its own vnode-based load entrypoint. */
  if (vn->xip_addr == NULL &&
      elf16_loader.detect(header, header_len, file_size, path)) {
    rc = elf16_load_vnode(p, vn, file_size, &native_cpu_ops, NULL, argv, 0);
    if (rc < 0) {
      mod_vfs.vnode_release(vn);
      return rc;
    }
    goto exec_loaded;
  }
#endif

  /* ── 3. Find a matching loader and resolve its CPU ops ─────────────── */
  const loader_t *matched = NULL;
  const cpu_ops_t *cpu_ops = NULL;
  for (int i = 0; loader_registry[i] != NULL; i++) {
    if (!loader_registry[i]->detect(header, header_len, file_size, path))
      continue;
    int arch = loader_registry[i]->required_arch_id;
    cpu_ops = (arch == 0 || arch == HOST_ARCH_ID) ? &native_cpu_ops
                                                  : cpu_ops_for_arch(arch);
    if (!cpu_ops) continue;
    matched = loader_registry[i];
    break;
  }
  if (!matched) {
    mod_vfs.vnode_release(vn);
    return rc;
  }

  /* ── 4. Dispatch.  load_vn streams from vn; legacy load() uses a
   *       staging buffer (and is being retired loader-by-loader). ──── */
  if (matched->load_vn) {
    rc = matched->load_vn(p, vn, file_size, cpu_ops, NULL, argv, 0);
    if (rc < 0) {
      mod_vfs.vnode_release(vn);
      return rc;
    }
  } else {
    proc_image_segment_t file_region = {0};
    uint8_t *file_buf = NULL;
    const uint8_t *file_base;

    if (vn->xip_addr == NULL) {
      if (mem_region_alloc(&file_region, PPAP_MEM_RAM_DATA, file_size,
                           PROC_IMAGE_SEG_WRITABLE) < 0) {
        mod_vfs.vnode_release(vn);
        return -(int)ENOMEM;
      }
      file_buf = (uint8_t *)file_region.base;

      if (!vn->mount || !vn->mount->ops || !vn->mount->ops->read) {
        mem_region_free(&file_region);
        mod_vfs.vnode_release(vn);
        return -(int)ENOEXEC;
      }

      long n = exec_read_from(vn, file_buf, file_size);
      if (n < 0 || (uint32_t)n != file_size) {
        mem_region_free(&file_region);
        mod_vfs.vnode_release(vn);
        return (n < 0) ? (int)n : -(int)ENOEXEC;
      }

      file_base = file_buf;
    } else {
      file_base = (const uint8_t *)vn->xip_addr;
    }

    uint32_t exec_flags = (vn->xip_addr != NULL) ? EXEC_FLAG_XIP_SOURCE : 0;
    rc =
        matched->load(p, file_base, file_size, cpu_ops, NULL, argv, exec_flags);
    if (rc < 0) {
      if (file_buf) mem_region_free(&file_region);
      mod_vfs.vnode_release(vn);
      return rc;
    }
    if (file_buf && !matched->xip) mem_region_free(&file_region);
  }

exec_loaded:
  __attribute__((unused));

  /* ── 5. Set process metadata ───────────────────────────────────────── */
  {
    const char *base = path;
    for (const char *s = path; *s; s++) {
      if (*s == '/') base = s + 1;
    }
    size_t clen = strlen(base);
    if (clen > 15) clen = 15;
    memcpy(p->comm, base, clen);
    p->comm[clen] = '\0';
  }

  if (current)
    memcpy(p->cwd, current->cwd, sizeof(p->cwd));
  else
    strcpy(p->cwd, "/");

  for (int i = 0; i < NSIG; i++) {
    if (p->sig_handlers[i] != SIG_IGN) p->sig_handlers[i] = SIG_DFL;
  }
  p->sig_pending = 0;
  p->sig_blocked = 0;

  mod_vfs.vnode_release(vn);
  return 0;
}
