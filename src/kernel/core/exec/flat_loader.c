/*
 * flat_loader.c — Flat binary loader for i16 (8086 real mode)
 *
 * Loads headerless flat binaries (like DOS .COM files) into a single
 * page and sets up the process to execute from offset 0.
 *
 * Detection: any file with .com extension, or any file ≤4 KB that
 * starts with a valid 8086 instruction (not ELF/script magic).
 *
 * Memory layout after load:
 *   tracked page-backed slot 0 = flat binary + stack (4 KB page)
 *   Entry point: page base + 0
 *   Stack: top of page (grows down)
 */

#include <string.h>

#include "common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/arch.h"
#include "kernel/core/cpu/cpu.h"
#include "kernel/core/exec/exec_args.h"
#include "kernel/core/exec/loader.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/mm/page_io.h"
#include "kernel/core/mm/region.h"
#include "kernel/core/proc/proc.h"

#define FLAT_MAX_SIZE (PAGE_SIZE - 256) /* leave room for stack */

/* ── Detection ─────────────────────────────────────────────────────────── */

static int flat_detect(const uint8_t *header, uint32_t header_len,
                       uint32_t file_size, const char *path) {
  /* Only on native i16 host */
  if (HOST_ARCH_ID != CPU_ARCH_8086) return 0;

  /* Check .com extension */
  if (path) {
    int len = 0;
    while (path[len]) len++;
    if (len >= 4 && (path[len - 4] == '.' &&
                     (path[len - 3] == 'c' || path[len - 3] == 'C') &&
                     (path[len - 2] == 'o' || path[len - 2] == 'O') &&
                     (path[len - 1] == 'm' || path[len - 1] == 'M')))
      return 1;
  }

  /* Accept any small file that doesn't start with ELF or script magic */
  if (file_size > 0 && file_size <= FLAT_MAX_SIZE && header_len >= 2) {
    if (header[0] == 0x7F && header[1] == 'E') return 0; /* ELF */
    if (header[0] == '#' && header[1] == '!') return 0;  /* script */
    return 1;
  }

  return 0;
}

/* ── Loading ───────────────────────────────────────────────────────────── */

static int flat_load_vn(pcb_t *p, vnode_t *vn, uint32_t file_size,
                        const cpu_ops_t *cpu_ops, void *cpu_state,
                        const exec_args_t *args, uint32_t flags) {
  (void)flags;
  (void)cpu_ops;
  (void)cpu_state;
  (void)args;

  if (file_size > FLAT_MAX_SIZE) return -ENOMEM;

  /* Allocate one page for code + data + stack */
  page_id_t pid = page_alloc();
  if (pid == PAGE_ID_INVALID) return -ENOMEM;

  /* Stream the binary into the page; any tail is zeroed below. */
  long nread = mod_vfs.vnode_read(vn, pid, 0, file_size, 0);
  if (nread < 0 || (uint32_t)nread != file_size) {
    page_free(pid);
    return (nread < 0) ? (int)nread : -ENOEXEC;
  }

  /* Zero the remaining stack area after the binary. */
  if (file_size < PAGE_SIZE)
    page_zero(pid, (uint16_t)file_size, PAGE_SIZE - file_size);

  if (proc_track_page(p, 0, pid) < 0) {
    page_free(pid);
    return -ENOMEM;
  }

  /* Entry point: start of page. Stack: top of page. */
  uint32_t base_linear = page_linear(pid);
  void *page = (void *)(uintptr_t)base_linear;
  void (*entry)(void) = (void (*)(void))page;
  uintptr_t user_sp = (uintptr_t)page + PAGE_SIZE;

  proc_setup_stack(p, entry, user_sp);
  p->image.text = proc_image_segment_make(page, file_size, PPAP_MEM_RAM_TEXT,
                                          PROC_IMAGE_SEG_EXECUTABLE);
  p->image.text.base_page = pid;
  p->image.data = proc_image_segment_make(page, PAGE_SIZE, PPAP_MEM_RAM_DATA,
                                          PROC_IMAGE_SEG_WRITABLE);
  p->image.data.base_page = pid;
  p->image.entry = (uintptr_t)page;

  return 0;
}

/* ── Loader registration ──────────────────────────────────────────────── */

const loader_t flat_loader = {
    .name = "flat",
    .detect = flat_detect,
    .load = flat_load_vn,
    .required_arch_id = CPU_ARCH_8086,
};
