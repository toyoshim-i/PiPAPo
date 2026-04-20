/*
 * exec.c — execve coordinator for PPAP
 *
 * Reads the executable file header for loader detection, iterates the
 * loader registry to find a matching binary format, and delegates the
 * actual binary load to the matched loader via its vnode-based load()
 * entry point.  Post-load, sets process metadata (comm, cwd, signals).
 */

#include "kernel/core/exec/exec.h"

#include <string.h>

#include "common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/arch.h"
#include "kernel/core/exec/loader.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/signal/signal.h"

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

int exec_execve(pcb_t *p, const char *path, const char *const *argv,
                const char *const *envp) {
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

  /* ── 4. Dispatch.  Every registered loader streams directly from vn. */
  rc = matched->load(p, vn, file_size, cpu_ops, NULL, argv, envp, 0);
  if (rc < 0) {
    mod_vfs.vnode_release(vn);
    return rc;
  }

  /* Native and subsystem bridges consult the PCB's CPU access hooks after
   * exec, so publish the resolved cpu_ops on success even for native loads.
   * Native i16 memory helpers ignore cpu_state, so NULL is valid there. */
  p->cpu_ops = cpu_ops;
  p->cpu_state = NULL;

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
    p->sig_restorers[i] = (sighandler_t)0;
  }
  p->sig_pending = 0;
  p->sig_blocked = 0;

  mod_vfs.vnode_release(vn);
  return 0;
}
