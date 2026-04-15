/*
 * sos_loader.c — S-OS "SWORD" .obj binary loader
 *
 * Detects _SOS magic header and loads S-OS binaries into a Z80
 * emulator instance.  Memory allocation and eCPU initialization are
 * done here; the in-memory image setup is delegated to sos_host.c.
 */

#include "common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/cpu/ecpu_z80.h"
#include "kernel/core/exec/exec.h"
#include "kernel/core/exec/loader.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/subsys/sos/sos_bridge.h"
#include "kernel/core/subsys/sos/sos_host.h"
#include "kernel/core/subsys/subsys.h"
#if defined(__m68k__)
#include "kernel/common/ioregs.h"
#endif
#include <string.h>

/* ── Detection ─────────────────────────────────────────────────────────── */

static int sos_detect(const uint8_t *header, uint32_t header_len,
                      uint32_t file_size, const char *path) {
  (void)path;

  /* Must have at least the 18-byte _SOS header */
  if (file_size < SOS_HEADER_SIZE) return 0;
  if (header_len < SOS_HEADER_SIZE) return 0;

  /* Check _SOS magic */
  if (memcmp(header, SOS_MAGIC, SOS_MAGIC_LEN) != 0) return 0;

  /* Validate header structure: spaces at +4, +7, +12 and LF at +17 */
  if (header[4] != ' ' || header[7] != ' ' || header[12] != ' ') return 0;
  if (header[17] != 0x0A) return 0;

  return 1;
}

/* ── Loader ───────────────────────────────────────────────────────────── */

static int sos_load(pcb_t *p, const uint8_t *file_buf, uint32_t file_size,
                    const cpu_ops_t *cpu_ops, void *cpu_state,
                    const char *const *argv, uint32_t flags) {
  (void)flags;
  (void)cpu_ops;
  (void)cpu_state;

  proc_image_segment_t data_region = {0};
  proc_image_segment_t stack_region = {0};

  /* ── 1. Allocate Z80 memory (64KB) + state page ────────────────────── */
  if (mem_region_alloc(&data_region, PPAP_MEM_RAM_DATA,
                       (SOS_Z80_MEM_PAGES + 1u) * PAGE_SIZE,
                       PROC_IMAGE_SEG_WRITABLE) < 0) {
    return -(int)ENOMEM;
  }

  if (proc_track_page_range(p, 0, mem_region_ptr_to_page(data_region.base),
                            data_region.size / PAGE_SIZE) < 0) {
    mem_region_free(&data_region);
    return -(int)ENOMEM;
  }

  uint8_t *z80_mem = (uint8_t *)data_region.base;
  sos_exec_state_t *state =
      (sos_exec_state_t *)(z80_mem + SOS_Z80_MEM_PAGES * PAGE_SIZE);

  /* ── 2. Allocate stack page ────────────────────────────────────────── */
  if (mem_region_alloc(&stack_region, PPAP_MEM_RAM_STACK, PAGE_SIZE,
                       PROC_IMAGE_SEG_WRITABLE | PROC_IMAGE_SEG_OWNED) < 0) {
    proc_release_tracked_pages(p, 0, SOS_Z80_MEM_PAGES + 1u);
    return -(int)ENOMEM;
  }
  p->stack_page_id = mem_region_ptr_to_page(stack_region.base);
  p->image.stack = stack_region;

  /* ── 3. Initialize Z80 emulator ────────────────────────────────────── */
  memset(state, 0, sizeof(*state));
  ecpu_z80_ops.init((cpu_state_t *)&state->z80, z80_mem, 65536);

  /* Set up trap handler — only RST 0 and RST 18h are intercepted */
  ecpu_z80_ops.set_trap_handler((cpu_state_t *)&state->z80, sos_trap_handler,
                                &state->sos);

  /* ── 4. Build the S-OS memory image ─────────────────────────────────── */
  {
    const char *path = (argv && argv[0]) ? argv[0] : "";
    int rc = sos_load_obj(&state->z80, &state->sos, file_buf, file_size, path);
    if (rc < 0) {
      proc_release_tracked_pages(p, 0, SOS_Z80_MEM_PAGES + 1u);
      return rc;
    }
  }

  /* ── 5. Process image metadata ─────────────────────────────────────── */
  {
    uint16_t load_addr =
        state->z80.memory[SOS_DTADR] | (state->z80.memory[SOS_DTADR + 1] << 8);
    uint16_t payload_size =
        state->z80.memory[SOS_SIZE] | (state->z80.memory[SOS_SIZE + 1] << 8);
    p->image.text =
        proc_image_segment_make(z80_mem + load_addr, payload_size,
                                PPAP_MEM_RAM_TEXT, PROC_IMAGE_SEG_EXECUTABLE);
  }
  p->image.data = data_region;
  p->image.entry = state->z80.pc;

  /* ── 6. Tag as S-OS process ────────────────────────────────────────── */
  p->subsys = SUBSYS_SOS;
  p->subsys_data = state;

  {
    const subsys_ops_t *ops = subsys_ops_table[SUBSYS_SOS];
    if (ops && ops->on_init) ops->on_init(p);
  }

  /* ── 7. Set up kernel-mode entry point ─────────────────────────────── */
  proc_setup_stack(p, sos_run_process, 0);

#if defined(__m68k__)
  {
    uint8_t *exc = (uint8_t *)(uintptr_t)p->sp + 15u * sizeof(uint32_t);
    *(uint16_t *)(void *)exc = SR_SUPV_IRQ;
    p->usp = (uint32_t)(uintptr_t)mem_region_page_to_ptr(p->stack_page_id) +
             PAGE_SIZE;
  }
#endif

  return 0;
}

static int sos_load_vn(pcb_t *p, vnode_t *vn, uint32_t file_size,
                       const cpu_ops_t *cpu_ops, void *cpu_state,
                       const char *const *argv, uint32_t flags) {
  /* sos_loader uses the Z80 emulator on m68k/RV/ARM — all flat-pointer
   * arches — so staging is safe. */
  proc_image_segment_t staging = {0};
  if (mem_region_alloc(&staging, PPAP_MEM_RAM_DATA, file_size,
                       PROC_IMAGE_SEG_WRITABLE) < 0)
    return -(int)ENOMEM;

  uintptr_t addr = (uintptr_t)staging.base;
  page_id_t page = (page_id_t)(addr / PAGE_SIZE);
  uint16_t page_off = (uint16_t)(addr & (PAGE_SIZE - 1u));
  long n = mod_vfs.vnode_read(vn, page, page_off, file_size, 0);
  if (n < 0 || (uint32_t)n != file_size) {
    mem_region_free(&staging);
    return (n < 0) ? (int)n : -(int)ENOEXEC;
  }

  int rc = sos_load(p, (const uint8_t *)staging.base, file_size, cpu_ops,
                    cpu_state, argv, flags);
  mem_region_free(&staging);
  return rc;
}

/* ── Loader registration ───────────────────────────────────────────────── */

const loader_t sos_loader = {
    .name = "sos",
    .detect = sos_detect,
    .load = sos_load_vn,
    .required_arch_id = CPU_ARCH_Z80,
};
