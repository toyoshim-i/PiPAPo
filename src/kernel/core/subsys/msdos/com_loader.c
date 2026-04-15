/*
 * com_loader.c --- MS-DOS .COM binary loader
 *
 * Detects .COM files, allocates the 64 KB DOS segment, and delegates
 * the in-memory image setup (PSP, binary copy, user-mode frame) to
 * dos_host.c.
 *
 * On native i16 the program runs directly; INT 21h is trapped by the
 * dos_trap.S ISR.  On other architectures the i8086 eCPU would be used
 * (not yet implemented).
 */

#include "kernel/core/subsys/msdos/com_loader.h"

#include "common/errno.h"
#include "kernel/common/core/subsys_info.h"
#include "kernel/core/cpu/cpu.h"
#include "kernel/core/exec/loader.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/subsys/msdos/dos_bridge.h"
#include "kernel/core/subsys/msdos/dos_host.h"
#include "kernel/core/subsys/subsys.h"

/* ── Detection ─────────────────────────────────────────────────────────── */

static int com_detect(const uint8_t *header, uint32_t header_len,
                      uint32_t file_size, const char *path) {
  (void)header;
  (void)header_len;

  if (file_size == 0 || file_size > DOS_COM_MAX_SIZE) return 0;

  /* Check for .COM extension (case-insensitive) */
  if (!path) return 0;
  int len = 0;
  while (path[len]) len++;
  if (len < 5) return 0; /* at least "/x.COM" */

  const char *ext = path + len - 4;
  if (ext[0] == '.' && (ext[1] == 'C' || ext[1] == 'c') &&
      (ext[2] == 'O' || ext[2] == 'o') && (ext[3] == 'M' || ext[3] == 'm'))
    return 1;

  return 0;
}

/* ── Loading ───────────────────────────────────────────────────────────── */

static int com_load_vn(pcb_t *p, vnode_t *vn, uint32_t file_size,
                       const cpu_ops_t *cpu_ops, void *cpu_state,
                       const char *const *argv, uint32_t flags) {
  (void)cpu_ops;
  (void)cpu_state;
  (void)flags;

  if (file_size > DOS_COM_MAX_SIZE) return -(int)ENOEXEC;

  /* 1. Allocate a 64 KB segment (16 contiguous pages) */
  page_id_t base_id = mem_region_page_alloc_contiguous(DOS_SEG_PAGES);
  if (base_id == PAGE_ID_INVALID) return -(int)ENOMEM;

  uint32_t base_linear = mem_region_page_linear(base_id);
  uint16_t proc_seg = (uint16_t)(base_linear >> 4);

  /* 2. Stream the .COM binary into the DOS segment and build the
   *    PSP + initial user frame. */
  uint16_t user_sp;
  int rc =
      dos_build_com_image(base_id, proc_seg, vn, file_size, argv, &user_sp);
  if (rc < 0) {
    for (uint16_t i = 0; i < DOS_SEG_PAGES; i++)
      mem_region_page_free(base_id + i);
    return rc;
  }

  /* 3. Tag as MS-DOS subsystem */
  p->subsys = SUBSYS_MSDOS;
  {
    const subsys_ops_t *ops = subsys_ops_table[SUBSYS_MSDOS];
    if (ops && ops->on_init) ops->on_init(p);
  }

  /* 4. Store user SS:SP in PCB and build kernel stack frame */
  p->exec_user_ss = proc_seg;
  p->exec_user_sp = user_sp;
  {
    uint16_t ksp = p->kernel_stack_top;
    uint16_t *kstack = (uint16_t *)(uintptr_t)ksp;
    *--kstack = proc_seg; /* user_SS */
    *--kstack = user_sp;  /* user_SP */
    /* Reserve 34-byte vfork-save slot (matches trap.S subw $34,%sp) */
    kstack -= 17;
    p->sp = (uint32_t)(uintptr_t)kstack;
  }

  /* 5. Track pages and set image metadata */
  for (uint16_t i = 0; i < DOS_SEG_PAGES; i++)
    proc_track_page(p, i, base_id + i);

  p->image.text = proc_image_segment_make(
      (void *)(uintptr_t)(uint16_t)base_linear, file_size + 0x100,
      PPAP_MEM_RAM_TEXT, PROC_IMAGE_SEG_EXECUTABLE);
  p->image.text.base_page = base_id;
  p->image.data = proc_image_segment_make(
      (void *)(uintptr_t)(uint16_t)base_linear, DOS_SEG_BYTES,
      PPAP_MEM_RAM_DATA, PROC_IMAGE_SEG_WRITABLE | PROC_IMAGE_SEG_OWNED);
  p->image.data.base_page = base_id;
  p->image.entry = 0x0100;
  p->brk_base = (file_size + 0x100 + 15u) & ~15u;
  p->brk_current = p->brk_base;
  p->ticks_remaining = PROC_DEFAULT_TICKS;

  return 0;
}

/* ── Loader registration ──────────────────────────────────────────────── */

const loader_t com_loader = {
    .name = "dos_com",
    .detect = com_detect,
    .load = com_load_vn,
    .required_arch_id = CPU_ARCH_8086,
};
