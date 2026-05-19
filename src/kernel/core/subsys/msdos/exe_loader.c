/*
 * exe_loader.c --- MS-DOS .EXE (MZ) binary loader
 *
 * Detects MZ .EXE files, reads the header, allocates a contiguous run
 * large enough for PSP + image + min_alloc, and delegates in-memory
 * setup to dos_build_exe_image() in dos_host.c.
 *
 * As with com_loader, the program runs directly on native i16; INT 21h
 * is trapped by dos_trap.S.  Relocations (hdr.reloc_count > 0) are
 * applied inside dos_build_exe_image by dos_apply_relocations.
 */

#include "kernel/core/subsys/msdos/exe_loader.h"

#include "common/errno.h"
#include "kernel/common/core/subsys_info.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/cpu/cpu.h"
#include "kernel/core/exec/exec_args.h"
#include "kernel/core/exec/loader.h"
#include "kernel/core/mm/region.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/subsys/msdos/dos_bridge.h"
#include "kernel/core/subsys/msdos/dos_host.h"
#include "kernel/core/subsys/subsys.h"

#define MZ_SIG_MZ 0x5A4D /* little-endian "MZ" */
#define MZ_SIG_ZM 0x4D5A /* little-endian "ZM" (old-linker variant) */

/* ── Detection ─────────────────────────────────────────────────────────── */

static int exe_detect(const uint8_t *header, uint32_t header_len,
                      uint32_t file_size, const char *path) {
  (void)path;

  if (file_size < sizeof(mz_header_t)) return 0;
  if (header_len < 2) return 0;

  uint16_t sig = (uint16_t)header[0] | ((uint16_t)header[1] << 8);
  return (sig == MZ_SIG_MZ || sig == MZ_SIG_ZM) ? 1 : 0;
}

/* ── Loading ───────────────────────────────────────────────────────────── */

static void exe_free_run(page_id_t base_id, uint32_t got_pages) {
  for (uint32_t i = 0; i < got_pages; i++) page_free(base_id + (page_id_t)i);
}

static int exe_load_vn(pcb_t *p, vnode_t *vn, uint32_t file_size,
                       const cpu_ops_t *cpu_ops, void *cpu_state,
                       const exec_args_t *args, uint32_t flags) {
  (void)flags;

  if (file_size < sizeof(mz_header_t)) return -(int)ENOEXEC;

  p->cpu_ops = cpu_ops;
  p->cpu_state = cpu_state;

  /* 1. Grab the largest contiguous run we can, bounded by
   *    DOS_SEG_PAGES_MAX.  The MZ header tells us the exact minimum,
   *    but we need scratch pages to read the header into first. */
  uint32_t got_pages = 0;
  page_id_t base_id =
      page_alloc_largest(DOS_SEG_PAGES, DOS_SEG_PAGES_MAX, &got_pages);
  if (base_id == PAGE_ID_INVALID) return -(int)ENOMEM;

  uint32_t base_linear = page_linear(base_id);
  /* Paragraph 0 of the run holds the process's MCB; the PSP starts
   * at paragraph 1 (= base_linear + DOS_MCB_BYTES). */
  uint16_t proc_seg = (uint16_t)((base_linear >> 4) + 1u);

  /* 2. Read MZ header into base_id:0 as scratch, then copy out.  The
   *    PSP built below will overwrite this area. */
  long hn = mod_vfs.vnode_read(vn, base_id, 0, sizeof(mz_header_t), 0);
  if (hn < 0) {
    exe_free_run(base_id, got_pages);
    return (int)hn;
  }
  if ((uint32_t)hn != sizeof(mz_header_t)) {
    exe_free_run(base_id, got_pages);
    return -(int)ENOEXEC;
  }

  mz_header_t hdr;
  page_read(base_id, 0, &hdr, sizeof(hdr));

  /* 3. Validate header and compute image size.  dos_build_exe_image()
   *    repeats these checks; doing them here lets us reject before
   *    touching the run. */
  if (hdr.header_size < 2 || hdr.page_count == 0) {
    exe_free_run(base_id, got_pages);
    return -(int)ENOEXEC;
  }
  uint32_t header_bytes = (uint32_t)hdr.header_size * 16u;
  if (header_bytes > file_size) {
    exe_free_run(base_id, got_pages);
    return -(int)ENOEXEC;
  }
  uint32_t image_bytes = (uint32_t)hdr.page_count * 512u;
  if (hdr.last_page_size != 0)
    image_bytes -= (512u - (uint32_t)hdr.last_page_size);
  if (image_bytes < header_bytes) {
    exe_free_run(base_id, got_pages);
    return -(int)ENOEXEC;
  }
  uint32_t code_size = image_bytes - header_bytes;

  /* 4. Check the allocation is big enough: MCB + PSP + code + min_alloc
   *    headroom, rounded up to whole pages. */
  uint32_t total_bytes =
      DOS_MCB_BYTES + 0x100u + code_size + (uint32_t)hdr.min_alloc * 16u;
  uint32_t pages_needed = (total_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
  if (pages_needed > got_pages) {
    exe_free_run(base_id, got_pages);
    return -(int)ENOMEM;
  }

  /* 5. Build the image (zero + stream + relocs + PSP + initial frame). */
  uint16_t user_ss;
  uint16_t user_sp;
  int rc = dos_build_exe_image(base_id, proc_seg, got_pages, vn, file_size,
                               &hdr, args, &user_ss, &user_sp);
  if (rc < 0) {
    exe_free_run(base_id, got_pages);
    return rc;
  }

  /* 6. Tag as MS-DOS subsystem */
  p->subsys = SUBSYS_MSDOS;
  {
    const subsys_ops_t *ops = subsys_ops_table[SUBSYS_MSDOS];
    if (ops && ops->on_init) ops->on_init(p);
  }

  dos_set_exec_dir(p, args);
  dos_apply_dbg_env(p, args);

  /* 7. Kernel-stack frame that trap.S's restore tail will pop. */
  p->exec_user_ss = user_ss;
  p->exec_user_sp = user_sp;
  {
    uint16_t ksp = p->kernel_sp;
    uint16_t *kstack = (uint16_t *)(uintptr_t)ksp;
    *--kstack = user_ss; /* user_SS */
    *--kstack = user_sp; /* user_SP */
    /* Reserve 34-byte vfork-save slot (matches trap.S subw $34,%sp) */
    kstack -= 17;
    p->sp = (uint32_t)(uintptr_t)kstack;
  }

  /* 8. Track pages and set image metadata.  See com_loader.c for the
   *    rationale on tracking only the first DOS_SEG_PAGES: user_pages[]
   *    is brk/mmap bookkeeping for the first 64 KB segment; pages past
   *    that are reached via image.data.{base_page,size} and still freed
   *    on exit through image_release_owned_segments(). */
  uint16_t track_pages = (got_pages < DOS_SEG_PAGES) ? (uint16_t)got_pages
                                                     : (uint16_t)DOS_SEG_PAGES;
  for (uint16_t i = 0; i < track_pages; i++) proc_track_page(p, i, base_id + i);

  uint32_t load_seg = (uint32_t)proc_seg + 0x10u;
  p->image.text = proc_image_segment_make(
      (void *)(uintptr_t)(uint16_t)(load_seg << 4), code_size,
      PPAP_MEM_RAM_TEXT, PROC_IMAGE_SEG_EXECUTABLE);
  p->image.text.base_page = base_id;
  p->image.data = proc_image_segment_make(
      (void *)(uintptr_t)(uint16_t)base_linear, got_pages * PAGE_SIZE,
      PPAP_MEM_RAM_DATA, PROC_IMAGE_SEG_WRITABLE | PROC_IMAGE_SEG_OWNED);
  p->image.data.base_page = base_id;
  p->image.entry = 0x100u + ((uint32_t)hdr.init_cs << 4) + hdr.init_ip;
  p->brk_base = (0x100u + code_size + 15u) & ~15u;
  p->brk_current = p->brk_base;
  p->ticks_remaining = PROC_DEFAULT_TICKS;

  return 0;
}

/* ── Loader registration ──────────────────────────────────────────────── */

const loader_t exe_loader = {
    .name = "dos_exe",
    .detect = exe_detect,
    .load = exe_load_vn,
    .required_arch_id = CPU_ARCH_8086,
};
