/*
 * cpm_loader.c — CP/M .COM binary format loader
 *
 * Detects .COM files by extension and loads them into a Z80 emulator
 * instance for execution.  Memory allocation, Z80 initialization, and
 * subsystem setup are coordinated here; the actual binary loading logic
 * is delegated to cpm_loader.c.
 *
 * See docs/subsystems/cpm.md §4 for the full design.
 */

#include "kernel/core/subsys/cpm/cpm_loader.h"

#include <string.h>

#include "common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/cpu/ecpu_z80.h"
#include "kernel/core/exec/exec.h"
#include "kernel/core/exec/exec_args.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/mm/region.h"
#include "kernel/core/subsys/cpm/cpm_bridge.h"
#include "kernel/core/subsys/cpm/cpm_host.h"
#include "kernel/core/subsys/subsys.h"

/* Z80 address space: 64KB = 16 × 4KB pages */
#define Z80_MEM_PAGES 16

/* ── Detection ─────────────────────────────────────────────────────────── */

static int cpm_detect(const uint8_t *header, uint32_t header_len,
                      uint32_t file_size, const char *path) {
  (void)header;
  (void)header_len;

  if (file_size > CPM_MAX_COM_SIZE || file_size == 0) return 0;

  /* Check for .COM or .com extension */
  size_t len = strlen(path);
  if (len < 5) return 0;

  const char *ext = path + len - 4;
  if ((ext[0] == '.') && (ext[1] == 'C' || ext[1] == 'c') &&
      (ext[2] == 'O' || ext[2] == 'o') && (ext[3] == 'M' || ext[3] == 'm'))
    return 1;

  return 0;
}

/* ── Helpers ───────────────────────────────────────────────────────────── */

/*
 * Per-process CP/M execution state -- stored in subsys_data.
 * Allocated from a separate page since it doesn't fit in Z80 address space.
 */
typedef struct {
  z80_state_t z80;
  cpm_state_t cpm;
} cpm_exec_state_t;

_Static_assert(sizeof(cpm_exec_state_t) <= PAGE_SIZE,
               "cpm_exec_state_t must fit in one page");

static void cpm_set_drive_a_root(cpm_state_t *cpm, const char *path) {
  const char *slash = NULL;
  uint32_t len = 0;

  cpm->drive_a_root[0] = 0;
  if (!path || !*path) return;

  for (const char *s = path; *s; s++) {
    if (*s == '/') slash = s;
  }

  if (!slash) return;
  if (slash == path) {
    cpm->drive_a_root[0] = '/';
    cpm->drive_a_root[1] = 0;
    return;
  }

  len = (uint32_t)(slash - path);
  if (len >= sizeof(cpm->drive_a_root)) len = sizeof(cpm->drive_a_root) - 1;
  memcpy(cpm->drive_a_root, path, len);
  cpm->drive_a_root[len] = 0;
}

/* ── Loader ────────────────────────────────────────────────────────────── */

static int cpm_load_vn(pcb_t *p, vnode_t *vn, uint32_t file_size,
                       const cpu_ops_t *cpu_ops, void *cpu_state,
                       const exec_args_t *args, uint32_t flags) {
  (void)flags;
  (void)cpu_ops;
  (void)cpu_state;

  proc_image_segment_t data_region = {0};
  proc_image_segment_t stack_region = {0};
  proc_image_segment_t staging = {0};

  /* ── 0. Stage the .COM file in RAM so cpm_load_com can memcpy it into
   *      emulated Z80 memory.  cpm_loader runs on flat-pointer arches
   *      only (Z80 emu on m68k/RV/ARM), so staging is safe. */
  if (image_segment_alloc(&staging, PPAP_MEM_RAM_DATA, file_size,
                          PROC_IMAGE_SEG_WRITABLE) < 0) {
    return -(int)ENOMEM;
  }
  {
    uintptr_t addr = (uintptr_t)staging.base;
    page_id_t pg = (page_id_t)(addr / PAGE_SIZE);
    uint16_t pg_off = (uint16_t)(addr & (PAGE_SIZE - 1u));
    long n = mod_vfs.vnode_read(vn, pg, pg_off, file_size, 0);
    if (n < 0 || (uint32_t)n != file_size) {
      image_segment_free(&staging);
      return (n < 0) ? (int)n : -(int)ENOEXEC;
    }
  }
  const uint8_t *file_buf = (const uint8_t *)staging.base;

  /* ── 1. Allocate Z80 memory (64KB contiguous) + state page ─────────── */
  if (image_segment_alloc(&data_region, PPAP_MEM_RAM_DATA,
                          (Z80_MEM_PAGES + 1u) * PAGE_SIZE,
                          PROC_IMAGE_SEG_WRITABLE) < 0) {
    image_segment_free(&staging);
    return -(int)ENOMEM;
  }

  if (proc_track_page_range(p, 0, page_from_ptr(data_region.base),
                            data_region.size / PAGE_SIZE) < 0) {
    image_segment_free(&data_region);
    image_segment_free(&staging);
    return -(int)ENOMEM;
  }

  uint8_t *z80_mem = (uint8_t *)data_region.base;
  cpm_exec_state_t *state =
      (cpm_exec_state_t *)(z80_mem + Z80_MEM_PAGES * PAGE_SIZE);

  /* ── 2. Allocate stack page ────────────────────────────────────────── */
  if (image_segment_alloc(&stack_region, PPAP_MEM_RAM_STACK, PAGE_SIZE,
                          PROC_IMAGE_SEG_WRITABLE | PROC_IMAGE_SEG_OWNED) < 0) {
    proc_release_tracked_pages(p, 0, Z80_MEM_PAGES + 1u);
    image_segment_free(&staging);
    return -(int)ENOMEM;
  }
  p->stack_page_id = page_from_ptr(stack_region.base);
  p->image.stack = stack_region;

  /* ── 3. Initialize Z80 emulator ────────────────────────────────────── */
  memset(state, 0, sizeof(*state));
  ecpu_z80_ops.init((cpu_state_t *)&state->z80, z80_mem, 65536);

  /* Set up trap handler for BDOS/BIOS interception */
  ecpu_z80_ops.set_trap_handler((cpu_state_t *)&state->z80, cpm_trap_handler,
                                &state->cpm);

  /* ── 4. Build command line from argv ───────────────────────────────
   * cpm_loader runs only on 32-bit arches (Z80 emulator host); the
   * 16 KB MSP easily accommodates a 128 B cmdline buffer here. */
  char cmdline[128];
  cmdline[0] = '\0';
  {
    int pos = 0;
    for (int i = 1; i < (int)args->argc && pos < 126; i++) {
      if (i > 1 && pos < 126) cmdline[pos++] = ' ';
      uint16_t alen = exec_args_argv_len(args, i);
      if (pos + (int)alen > 126) alen = (uint16_t)(126 - pos);
      exec_args_argv_copy(args, i, cmdline + pos, (uint16_t)(alen + 1u));
      pos += (int)alen;
    }
    cmdline[pos] = '\0';
  }

  /* ── 5. Load .COM binary into Z80 memory ──────────────────────────── */
  /* Extract path from args for drive_a_root */
  char path[VFS_PATH_MAX];
  if (exec_args_path(args, path, sizeof(path)) < 0) path[0] = '\0';
  cpm_load_com(&state->z80, &state->cpm, file_buf, file_size, cmdline);
  image_segment_free(&staging);
  cpm_set_drive_a_root(&state->cpm, path);
  p->image.text =
      proc_image_segment_make(z80_mem + CPM_TPA_BASE, file_size,
                              PPAP_MEM_RAM_TEXT, PROC_IMAGE_SEG_EXECUTABLE);
  p->image.data = data_region;
  p->image.entry = CPM_TPA_BASE;

  /* ── 6. Tag as CP/M process ────────────────────────────────────────── */
  p->subsys = SUBSYS_CPM;
  p->subsys_data = state;

  {
    const subsys_ops_t *ops = subsys_ops_table[SUBSYS_CPM];
    if (ops && ops->on_init) ops->on_init(p);
  }

  /* ── 7. Set up kernel-mode entry point ─────────────────────────────
   *
   * Unlike ELF which runs in user mode, CP/M .COM programs run via
   * the Z80 emulator in kernel mode.  proc_setup_kernel_stack() builds
   * a frame whose restored privilege level is supervisor / M-mode so
   * the run loop can touch IRQ-save helpers without trapping.
   */
  proc_setup_kernel_stack(p, cpm_run_process);

  return 0;
}

/* ── Loader registration ───────────────────────────────────────────────── */

const loader_t cpm_loader = {
    .name = "com",
    .detect = cpm_detect,
    .load = cpm_load_vn,
    .required_arch_id = CPU_ARCH_Z80,
};
