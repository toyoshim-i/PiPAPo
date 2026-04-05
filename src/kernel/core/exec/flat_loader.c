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

#include "kernel/core/exec/loader.h"

#include <string.h>

#include "common/errno.h"
#include "kernel/core/arch.h"
#include "kernel/core/cpu/cpu.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/proc/proc.h"

#define FLAT_MAX_SIZE (PAGE_SIZE - 256)  /* leave room for stack */

/* ── Detection ─────────────────────────────────────────────────────────── */

static int flat_detect(const uint8_t *buf, uint32_t size, const char *path)
{
  (void)buf;

  /* Only on native i16 host */
  if (HOST_ARCH_ID != CPU_ARCH_8086) return 0;

  /* Check .com extension */
  if (path) {
    int len = 0;
    while (path[len]) len++;
    if (len >= 4 &&
        (path[len-4] == '.' &&
         (path[len-3] == 'c' || path[len-3] == 'C') &&
         (path[len-2] == 'o' || path[len-2] == 'O') &&
         (path[len-1] == 'm' || path[len-1] == 'M')))
      return 1;
  }

  /* Accept any small file that doesn't start with ELF or script magic */
  if (size > 0 && size <= FLAT_MAX_SIZE) {
    if (buf[0] == 0x7F && buf[1] == 'E') return 0; /* ELF */
    if (buf[0] == '#' && buf[1] == '!')  return 0; /* script */
    return 1;
  }

  return 0;
}

/* ── Loading ───────────────────────────────────────────────────────────── */

static int flat_load(pcb_t *p, const uint8_t *file_buf, uint32_t file_size,
                     const cpu_ops_t *cpu_ops, void *cpu_state,
                     const char *const *argv, uint32_t flags)
{
  (void)flags;
  (void)cpu_ops;
  (void)cpu_state;
  (void)argv;

  if (file_size > FLAT_MAX_SIZE) return -ENOMEM;

  proc_image_segment_t page_region = {0};

  /* Allocate one page for code + data + stack */
  if (mem_region_alloc(&page_region, PPAP_MEM_RAM_DATA, PAGE_SIZE,
                       PROC_IMAGE_SEG_WRITABLE) < 0) {
    return -ENOMEM;
  }
  void *page = page_region.base;

  /* Copy binary to start of page */
  memcpy(page, file_buf, file_size);

  /* Zero remaining space (stack area) */
  memset((uint8_t *)page + file_size, 0, PAGE_SIZE - file_size);

  if (proc_track_page(p, 0, mem_region_ptr_to_page(page)) < 0) {
    mem_region_free(&page_region);
    return -ENOMEM;
  }

  /* Entry point: start of page. Stack: top of page. */
  void (*entry)(void) = (void (*)(void))page;
  uintptr_t user_sp = (uintptr_t)page + PAGE_SIZE;

  proc_setup_stack(p, entry, user_sp);
  p->image.text = proc_image_segment_make(page, file_size, PPAP_MEM_RAM_TEXT,
                                          PROC_IMAGE_SEG_EXECUTABLE);
  p->image.data = page_region;
  p->image.entry = (uintptr_t)page;

  return 0;
}

/* ── Loader registration ──────────────────────────────────────────────── */

const loader_t flat_loader = {
    .name = "flat",
    .detect = flat_detect,
    .load = flat_load,
    .required_arch_id = CPU_ARCH_8086,
    .xip = 0,
};
