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
/*
 * On i16, local stack buffers live in the shared DS=0 low-memory window, so
 * we can safely use a tiny header buffer there for ELF probing.
 */
extern const loader_t elf16_loader;
int elf16_load_vnode(pcb_t *p, vnode_t *vn, uint32_t file_size,
                     const cpu_ops_t *cpu_ops, void *cpu_state,
                     const char *const *argv, uint32_t flags);

static long exec_vnode_read_near(vnode_t *vn, void *buf, uint16_t len,
                                 uint32_t off) {
  page_id_t page;
  uint16_t page_off;

  /* TODO: replace inline ptr→page with proper page-indexed loader */
  uintptr_t addr = (uintptr_t)buf;
  page = (page_id_t)(addr / PAGE_SIZE);
  page_off = (uint16_t)(addr & (PAGE_SIZE - 1u));
  return mod_vfs.vnode_read(vn, page, page_off, len, off);
}
#endif

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

  /* ── 2. Read the file into memory (or use XIP address) ─────────────── */
  proc_image_segment_t file_region = {0};
  uint8_t *file_buf = NULL;
  const uint8_t *file_base;
  uint32_t file_size = vn->size;
  extern const loader_t *loader_registry[];
  const loader_t *matched_loader = NULL;
  int rc = -(int)ENOEXEC;

#if defined(__ia16__)
  if (vn->xip_addr == NULL && file_size >= sizeof(elf32_ehdr_t)) {
    elf32_ehdr_t ehdr;
    long hdr_read = exec_vnode_read_near(vn, &ehdr, sizeof(ehdr), 0);

    if (hdr_read == (long)sizeof(ehdr) &&
        elf16_loader.detect((const uint8_t *)&ehdr, file_size, path)) {
      rc = elf16_load_vnode(p, vn, file_size, &native_cpu_ops, NULL, argv, 0);
      if (rc < 0) {
        mod_vfs.vnode_release(vn);
        return rc;
      }
      matched_loader = &elf16_loader;
      goto exec_loaded;
    }
  }
#endif

  if (vn->xip_addr == NULL) {
    if (file_size == 0) {
      mod_vfs.vnode_release(vn);
      return -(int)ENOEXEC;
    }

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

    page_id_t page;
    uint16_t page_off;
    /* TODO: replace inline ptr→page with proper page-indexed loader */
    {
      uintptr_t addr = (uintptr_t)file_buf;
      page = (page_id_t)(addr / PAGE_SIZE);
      page_off = (uint16_t)(addr & (PAGE_SIZE - 1u));
    }
    long nread = mod_vfs.vnode_read(vn, page, page_off, file_size, 0);
    if (nread < 0 || (uint32_t)nread != file_size) {
      mem_region_free(&file_region);
      mod_vfs.vnode_release(vn);
      return (nread < 0) ? (int)nread : -(int)ENOEXEC;
    }

    file_base = file_buf;
  } else {
    file_base = (const uint8_t *)vn->xip_addr;
  }

  /* ── 3. Iterate loader registry ────────────────────────────────────── */
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
        continue;
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
    mod_vfs.vnode_release(vn);
    return rc;
  }

exec_loaded:
  __attribute__((unused));

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

  mod_vfs.vnode_release(vn);
  return 0;
}
