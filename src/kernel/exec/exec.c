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
#include "kernel/errno.h"
#include "kernel/mm/page.h"
#include "kernel/signal/signal.h"
#include "kernel/vfs/vfs.h"
#include "loader.h"

/* ── Contiguous page allocation helper ─────────────────────────────────── */

uint8_t *alloc_contiguous(uint32_t n_pages) {
  return page_alloc_contiguous(n_pages);
}

/* ── do_execve ─────────────────────────────────────────────────────────── */

int do_execve(pcb_t *p, const char *path, const char *const *argv) {
  vnode_t *vn = NULL;
  int err;

  const char *default_argv[2] = {path, NULL};
  if (!argv || !argv[0]) {
    argv = default_argv;
  }

  /* ── 1. Look up the binary in the VFS ──────────────────────────────── */
  err = vfs_lookup(path, &vn);
  if (err < 0) return err;

  if (vn->type != VNODE_FILE) {
    vnode_put(vn);
    return -(int)ENOEXEC;
  }

  /* ── 2. Read the file into memory (or use XIP address) ─────────────── */
  uint8_t *file_buf = NULL;
  uint32_t file_pages = 0;
  const uint8_t *file_base;
  uint32_t file_size = vn->size;

  if (vn->xip_addr == NULL) {
    if (file_size == 0) {
      vnode_put(vn);
      return -(int)ENOEXEC;
    }

    file_pages = (file_size + PAGE_SIZE - 1u) / PAGE_SIZE;
    file_buf = alloc_contiguous(file_pages);
    if (!file_buf) {
      vnode_put(vn);
      return -(int)ENOMEM;
    }

    if (!vn->mount || !vn->mount->ops || !vn->mount->ops->read) {
      for (uint32_t i = 0; i < file_pages; i++)
        page_free(file_buf + i * PAGE_SIZE);
      vnode_put(vn);
      return -(int)ENOEXEC;
    }

    long nread = vn->mount->ops->read(vn, file_buf, file_size, 0);
    if (nread < 0 || (uint32_t)nread != file_size) {
      for (uint32_t i = 0; i < file_pages; i++)
        page_free(file_buf + i * PAGE_SIZE);
      vnode_put(vn);
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
    if (loader_registry[i]->detect(file_base, file_size, path)) {
      int arch = loader_registry[i]->required_arch_id;
      const cpu_ops_t *cpu_ops;
      if (arch == 0 || arch == HOST_ARCH_ID)
        cpu_ops = &native_cpu_ops;
      else
        cpu_ops = cpu_ops_for_arch(arch);
      if (!cpu_ops) {
        rc = -(int)ENOEXEC;
        break;
      }

      rc = loader_registry[i]->load(p, file_base, file_size, cpu_ops, NULL,
                                    argv);
      if (rc == 0) matched_loader = loader_registry[i];
      break;
    }
  }

  if (!matched_loader) {
    if (file_buf) {
      for (uint32_t i = 0; i < file_pages; i++)
        page_free(file_buf + i * PAGE_SIZE);
    }
    vnode_put(vn);
    return rc;
  }

  /* ── 4. Free file buffer if the loader doesn't need it for XIP ───── */
  if (file_buf && !matched_loader->xip) {
    for (uint32_t i = 0; i < file_pages; i++)
      page_free(file_buf + i * PAGE_SIZE);
  }

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

  vnode_put(vn);
  return 0;
}
