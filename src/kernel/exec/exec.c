/*
 * exec.c — do_execve coordinator for PPAP
 *
 * Reads the executable file from the VFS, iterates the loader registry
 * to find a matching binary format, and delegates loading to the
 * matched loader.  Post-load, sets process metadata (comm, cwd,
 * signals) and manages the file buffer lifecycle.
 */

#include "exec.h"

#include <string.h>

#include "arch/arch.h"
#include "kernel/common/errno.h"
#include "kernel/klog.h"
#include "kernel/mm/mem_region.h"
#include "kernel/mm/page.h"
#include "kernel/signal/signal.h"
#include "kernel/common/mod/mod_vfs.h"
#include "loader.h"

/* ── do_execve ─────────────────────────────────────────────────────────── */

int do_execve(pcb_t *p, const char *path, const char *const *argv) {
  vnode_t *vn = NULL;
  int err;

  const char *default_argv[2] = {path, NULL};
  if (!argv || !argv[0]) {
    argv = default_argv;
  }

  /* ── 1. Look up the binary in the VFS ──────────────────────────────── */
#ifdef __ia16__
  klogf("EXEC: &vn=%lx path=%s\n", (unsigned long)(uintptr_t)&vn, path);
#endif
  err = mod_vfs.lookup(path, &vn);
#ifdef __ia16__
  klogf("EXEC: err=%u vn=%lx\n",
        (unsigned)(err < 0 ? -err : 0),
        (unsigned long)(uintptr_t)vn);
#endif
  if (err < 0) return err;

  if (vn->type != VNODE_FILE) {
    mod_vfs.rel_vnode(vn);
    return -(int)ENOEXEC;
  }

  /* ── 2. Read the file into memory (or use XIP address) ─────────────── */
  proc_image_segment_t file_region = {0};
  uint8_t *file_buf = NULL;
  const uint8_t *file_base;
  uint32_t file_size = vn->size;

  if (vn->xip_addr == NULL) {
    if (file_size == 0) {
      mod_vfs.rel_vnode(vn);
      return -(int)ENOEXEC;
    }

    if (mem_region_alloc(&file_region, PPAP_MEM_RAM_DATA, file_size,
                         PROC_IMAGE_SEG_WRITABLE) < 0) {
      mod_vfs.rel_vnode(vn);
      return -(int)ENOMEM;
    }
    file_buf = (uint8_t *)file_region.base;

    if (!vn->mount || !vn->mount->ops || !vn->mount->ops->read) {
#ifdef __ia16__
      klogf("EXEC: no read op (mount=%lx ops=%lx)\n",
            (unsigned long)(uintptr_t)vn->mount,
            (unsigned long)(uintptr_t)(vn->mount ? vn->mount->ops : 0));
#endif
      mem_region_free(&file_region);
      mod_vfs.rel_vnode(vn);
      return -(int)ENOEXEC;
    }

#ifdef __ia16__
    klogf("EXEC: reading %lu bytes (xip=%lx read=%lx)\n",
          (unsigned long)file_size,
          (unsigned long)(uintptr_t)vn->xip_addr,
          (unsigned long)(uintptr_t)vn->mount->ops->read);
#endif
    long nread = vn->mount->ops->read(vn, file_buf, file_size, 0);
    if (nread < 0 || (uint32_t)nread != file_size) {
      mem_region_free(&file_region);
      mod_vfs.rel_vnode(vn);
      return (nread < 0) ? (int)nread : -(int)ENOEXEC;
    }

    file_base = file_buf;
  } else {
    file_base = (const uint8_t *)vn->xip_addr;
  }

  /* ── 3. Iterate loader registry ────────────────────────────────────── */
  extern const loader_t *loader_registry[];
  const loader_t *matched_loader = NULL;
  int rc = -(int)ENOEXEC;

  for (int i = 0; loader_registry[i] != NULL; i++) {
    int det = loader_registry[i]->detect(file_base, file_size, path);
    if (det) {
      int arch = loader_registry[i]->required_arch_id;
      const cpu_ops_t *cpu_ops;
      if (arch == 0 || arch == HOST_ARCH_ID) {
        cpu_ops = &native_cpu_ops;
      } else {
        cpu_ops = cpu_ops_for_arch(arch);
      }
      if (!cpu_ops) {
        rc = -(int)ENOEXEC;
        break;
      }

      uint32_t exec_flags = (vn->xip_addr != NULL) ? EXEC_FLAG_XIP_SOURCE : 0;
      rc = loader_registry[i]->load(p, file_base, file_size, cpu_ops, NULL,
                                    argv, exec_flags);
      if (rc == 0) matched_loader = loader_registry[i];
      break;
    }
  }

  if (!matched_loader) {
    if (file_buf) mem_region_free(&file_region);
    mod_vfs.rel_vnode(vn);
    return rc;
  }

  /* ── 4. Free file buffer if the loader doesn't need it for XIP ───── */
  if (file_buf && !matched_loader->xip) mem_region_free(&file_region);

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

  mod_vfs.rel_vnode(vn);
  return 0;
}

/* ── Module definition ─────────────────────────────────────────────────── */

#include "kernel/common/mod/mod_exec.h"

mod_exec_t mod_exec = {
  .do_execve = do_execve,
};
