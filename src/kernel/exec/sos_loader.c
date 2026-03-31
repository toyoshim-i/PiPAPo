/*
 * sos_loader.c — S-OS "SWORD" .obj binary loader
 *
 * Detects _SOS magic header and loads S-OS binaries into a Z80
 * emulator instance for execution.  Memory allocation, Z80
 * initialization, and subsystem setup are coordinated here.
 */

#include "exec.h"
#include "kernel/cpu/ecpu_z80.h"
#include "kernel/common/errno.h"
#include "kernel/mm/mem_region.h"
#include "kernel/mm/page.h"
#include "kernel/subsys/sos_bridge.h"
#include "kernel/subsys/subsys.h"
#include "loader.h"
#if defined(__m68k__)
#include "arch/ioregs.h"
#endif
#include <string.h>

/* Z80 address space: 64KB = 16 × 4KB pages */
#define Z80_MEM_PAGES 16

/* ── Detection ─────────────────────────────────────────────────────────── */

static int sos_detect(const uint8_t *file_buf, uint32_t file_size,
                      const char *path) {
  (void)path;

  /* Must have at least the 18-byte _SOS header */
  if (file_size < SOS_HEADER_SIZE) return 0;

  /* Check _SOS magic */
  if (memcmp(file_buf, SOS_MAGIC, SOS_MAGIC_LEN) != 0) return 0;

  /* Validate header structure: spaces at +4, +7, +12 and LF at +17 */
  if (file_buf[4] != ' ' || file_buf[7] != ' ' || file_buf[12] != ' ') return 0;
  if (file_buf[17] != 0x0A) return 0;

  return 1;
}

/* ── Per-process S-OS execution state ─────────────────────────────────── */

typedef struct {
  z80_state_t z80;
  sos_state_t sos;
} sos_exec_state_t;

_Static_assert(sizeof(sos_exec_state_t) <= PAGE_SIZE,
               "sos_exec_state_t must fit in one page");

/* ── S-OS memory map initialization ───────────────────────────────────── */

static void sos_setup_memory(z80_state_t *cpu, sos_state_t *sos) {
  uint8_t *mem = cpu->memory;

  /* RST 00h at 0x0000: JP to cold start — trap intercepts */
  mem[0x0000] = 0xC3; /* JP */
  mem[0x0001] = SOS_COLD_ENTRY & 0xFF;
  mem[0x0002] = SOS_COLD_ENTRY >> 8;

  /* RST 18h at 0x0018: JP to self — trap intercepts before execution */
  mem[SOS_RST18_ADDR] = 0xC3; /* JP */
  mem[SOS_RST18_ADDR + 1] = SOS_RST18_ADDR & 0xFF;
  mem[SOS_RST18_ADDR + 2] = SOS_RST18_ADDR >> 8;

  /*
   * Internal RST stub area at SOS_STUB_BASE (0x0100).
   * Each function has a 2-byte stub: { RST 0; RET }.
   * RST 0 fires the trap; the handler computes the function index
   * from cpu->pc (= SOS_STUB_BASE + fn * 2 + 1).
   */
  for (int fn = 0; fn < SOS_FN_MAX; fn++) {
    uint16_t stub = SOS_STUB_BASE + fn * 2;
    mem[stub] = 0xC7;     /* RST 0 */
    mem[stub + 1] = 0xC9; /* RET */
  }

  /*
   * Monitor jump table (0x1F80–0x1FFD): JP to internal stub area.
   * Entries count downward: addr = 0x1FFD − fn × 3.
   */
  for (int fn = 0; fn <= 37; fn++) {
    uint16_t addr = SOS_MON_TOP - fn * 3;
    uint16_t stub = SOS_STUB_BASE + fn * 2;
    mem[addr] = 0xC3; /* JP */
    mem[addr + 1] = stub & 0xFF;
    mem[addr + 2] = stub >> 8;
  }

  /*
   * Extended API (0x2000–0x2036): JP to internal stub area.
   * Entries count upward: addr = 0x2000 + (fn − 40) × 3.
   */
  for (int fn = 40; fn < SOS_FN_MAX; fn++) {
    uint16_t addr = SOS_EXT_BASE + (fn - 40) * 3;
    uint16_t stub = SOS_STUB_BASE + fn * 2;
    mem[addr] = 0xC3; /* JP */
    mem[addr + 1] = stub & 0xFF;
    mem[addr + 2] = stub >> 8;
  }

  /* Initialize work area (0x1F5B–0x1F7F) */
  mem[SOS_MXLIN] = 25;                 /* screen height */
  mem[SOS_WIDTH] = 80;                 /* screen width */
  mem[SOS_DSK] = sos->current_session; /* current device */
  mem[SOS_DVSW] = 0;                   /* device: FDD */
  mem[SOS_LPSW] = 0;                   /* printer off */

  /* User startup address → #HOT */
  mem[SOS_USR] = 0xFA; /* 0x1FFA = #HOT */
  mem[SOS_USR + 1] = 0x1F;

  /* Stack pointer */
  mem[SOS_STKAD] = SOS_STACK_TOP & 0xFF;
  mem[SOS_STKAD + 1] = SOS_STACK_TOP >> 8;

  /* User RAM limit */
  mem[SOS_MEMAX] = 0x00;
  mem[SOS_MEMAX + 1] = 0xD0; /* D000h */

  /* Initialize file_fd */
  sos->file_fd = -1;

  /* Initialize screen state */
  sos->screen_width = 80;
  sos->screen_height = 25;
  sos->cursor_x = 0;
  sos->cursor_y = 0;
  __builtin_memset(sos->screen_buf, ' ', sizeof(sos->screen_buf));
}

/* ── Drive A root from argv[0] ─────────────────────────────────────────── */

static void sos_set_drive_a_root(sos_state_t *sos, const char *path) {
  const char *slash = NULL;
  uint32_t len = 0;

  sos->drive_a_root[0] = 0;
  if (!path || !*path) return;

  for (const char *s = path; *s; s++) {
    if (*s == '/') slash = s;
  }

  if (!slash) return;
  if (slash == path) {
    sos->drive_a_root[0] = '/';
    sos->drive_a_root[1] = 0;
    return;
  }

  len = (uint32_t)(slash - path);
  if (len >= sizeof(sos->drive_a_root)) len = sizeof(sos->drive_a_root) - 1;
  memcpy(sos->drive_a_root, path, len);
  sos->drive_a_root[len] = 0;
}

/* ── Loader ───────────────────────────────────────────────────────────── */

static int sos_load(pcb_t *p, const uint8_t *file_buf, uint32_t file_size,
                    const cpu_ops_t *cpu_ops, void *cpu_state,
                    const char *const *argv, uint32_t flags) {
  (void)flags;
  (void)cpu_ops;
  (void)cpu_state;

  /* ── 1. Parse _SOS header ──────────────────────────────────────────── */
  sos_header_t hdr;
  if (sos_parse_header(file_buf, file_size, &hdr) < 0) return -(int)ENOEXEC;

  /* Only binary mode is executable */
  if (hdr.file_mode != SOS_MODE_BINARY) return -(int)ENOEXEC;

  /* Payload is everything after the 18-byte header */
  const uint8_t *payload = file_buf + SOS_HEADER_SIZE;
  uint32_t payload_size = file_size - SOS_HEADER_SIZE;

  /* Validate load address */
  if (hdr.load_addr + payload_size > 65536)
    payload_size = 65536 - hdr.load_addr;

  proc_image_segment_t data_region = {0};
  proc_image_segment_t stack_region = {0};

  /* ── 2. Allocate Z80 memory (64KB) + state page ────────────────────── */
  if (mem_region_alloc(&data_region, PPAP_MEM_RAM_DATA,
                       (Z80_MEM_PAGES + 1u) * PAGE_SIZE,
                       PROC_IMAGE_SEG_WRITABLE) < 0) {
    return -(int)ENOMEM;
  }

  if (proc_track_page_range(p, 0, mm_ptr_to_page(data_region.base),
                            data_region.size / PAGE_SIZE) < 0) {
    mem_region_free(&data_region);
    return -(int)ENOMEM;
  }

  uint8_t *z80_mem = (uint8_t *)data_region.base;
  sos_exec_state_t *state =
      (sos_exec_state_t *)(z80_mem + Z80_MEM_PAGES * PAGE_SIZE);

  /* ── 3. Allocate stack page ────────────────────────────────────────── */
  if (mem_region_alloc(&stack_region, PPAP_MEM_RAM_STACK, PAGE_SIZE,
                       PROC_IMAGE_SEG_WRITABLE |
                           PROC_IMAGE_SEG_OWNED) < 0) {
    proc_release_tracked_pages(p, 0, Z80_MEM_PAGES + 1u);
    return -(int)ENOMEM;
  }
  p->stack_page_id = mm_ptr_to_page(stack_region.base);
  p->image.stack = stack_region;

  /* ── 4. Initialize Z80 emulator ────────────────────────────────────── */
  memset(state, 0, sizeof(*state));
  ecpu_z80_ops.init((cpu_state_t *)&state->z80, z80_mem, 65536);

  /* Set up trap handler — only RST 0 and RST 18h are intercepted */
  ecpu_z80_ops.set_trap_handler((cpu_state_t *)&state->z80, sos_trap_handler,
                                &state->sos);

  /* ── 5. Set drive A root from argv[0] ─────────────────────────────── */
  {
    const char *path = (argv && argv[0]) ? argv[0] : "";
    sos_set_drive_a_root(&state->sos, path);
  }

  /* ── 6. Zero memory and set up S-OS memory map ─────────────────────── */
  memset(z80_mem, 0, 65536);
  sos_setup_memory(&state->z80, &state->sos);

  /* ── 6. Load payload at header-specified load address ──────────────── */
  memcpy(&z80_mem[hdr.load_addr], payload, payload_size);

  /* Set initial CPU state */
  state->z80.pc = hdr.exec_addr;
  state->z80.sp = SOS_STACK_TOP; /* Stack at 0x0800, grows down */

  /* Push return address 0x0000 so RET triggers cold start (exit) */
  z80_push16(&state->z80, 0x0000);

  /* Set DTADR/SIZE/EXADR in work area */
  z80_mem[SOS_DTADR] = hdr.load_addr & 0xFF;
  z80_mem[SOS_DTADR + 1] = hdr.load_addr >> 8;
  z80_mem[SOS_SIZE] = payload_size & 0xFF;
  z80_mem[SOS_SIZE + 1] = (payload_size >> 8) & 0xFF;
  z80_mem[SOS_EXADR] = hdr.exec_addr & 0xFF;
  z80_mem[SOS_EXADR + 1] = hdr.exec_addr >> 8;
  p->image.text = proc_image_segment_make(z80_mem + hdr.load_addr,
                                          payload_size, PPAP_MEM_RAM_TEXT,
                                          PROC_IMAGE_SEG_EXECUTABLE);
  p->image.data = data_region;
  p->image.entry = hdr.exec_addr;

  /* ── 7. Tag as S-OS process ────────────────────────────────────────── */
  p->subsys = SUBSYS_SOS;
  p->subsys_data = state;

  {
    const subsys_ops_t *ops = subsys_ops_table[SUBSYS_SOS];
    if (ops && ops->on_init) ops->on_init(p);
  }

  /* ── 8. Set up kernel-mode entry point ─────────────────────────────── */
  proc_setup_stack(p, sos_run_process, 0);

#if defined(__m68k__)
  {
    uint8_t *exc = (uint8_t *)(uintptr_t)p->sp + 15u * sizeof(uint32_t);
    *(uint16_t *)(void *)exc = SR_SUPV_IRQ;
    p->usp = (uint32_t)(uintptr_t)mm_page_to_ptr(p->stack_page_id) + PAGE_SIZE;
  }
#endif

  return 0;
}

/* ── Loader registration ───────────────────────────────────────────────── */

const loader_t sos_loader = {
    .name = "sos",
    .detect = sos_detect,
    .load = sos_load,
    .required_arch_id = CPU_ARCH_Z80,
    .xip = 0,
};
