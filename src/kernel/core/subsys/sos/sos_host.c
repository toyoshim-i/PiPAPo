/*
 * sos_host.c — S-OS subsystem memory image setup
 *
 * Builds the emulated Z80 memory image for S-OS "SWORD" programs:
 *   - Zero-page vectors (RST 00h cold start, RST 18h trap)
 *   - RST stub area at 0x0100 (one 2-byte { RST 0; RET } per function)
 *   - Monitor jump table at 0x1F80–0x1FFD
 *   - Extended API jump table at 0x2000–0x2036
 *   - Work area defaults (0x1F5B–0x1F7F)
 *   - Payload copied at its header-specified load address
 *   - Initial CPU state (PC = exec_addr, SP = 0x0800, return addr 0)
 */

#include "kernel/core/subsys/sos/sos_host.h"

#include <string.h>

#include "common/errno.h"
#include "kernel/core/subsys/sos/sos_bridge.h"

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

/* ── Drive A root from executable path ────────────────────────────────── */

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

/* ── Main entry point ──────────────────────────────────────────────────── */

int sos_load_obj(z80_state_t *cpu, sos_state_t *sos, const uint8_t *file_buf,
                 uint32_t file_size, const char *path) {
  /* ── 1. Parse _SOS header ───────────────────────────────────────────── */
  sos_header_t hdr;
  if (sos_parse_header(file_buf, file_size, &hdr) < 0) return -(int)ENOEXEC;

  /* Only binary mode is executable */
  if (hdr.file_mode != SOS_MODE_BINARY) return -(int)ENOEXEC;

  /* Payload is everything after the 18-byte header */
  const uint8_t *payload = file_buf + SOS_HEADER_SIZE;
  uint32_t payload_size = file_size - SOS_HEADER_SIZE;

  /* Clamp load to 64 KB */
  if (hdr.load_addr + payload_size > 65536)
    payload_size = 65536 - hdr.load_addr;

  /* ── 2. Set drive A root from path ──────────────────────────────────── */
  sos_set_drive_a_root(sos, path);

  /* ── 3. Zero memory and set up S-OS memory map ──────────────────────── */
  memset(cpu->memory, 0, 65536);
  sos_setup_memory(cpu, sos);

  /* ── 4. Load payload at header-specified load address ───────────────── */
  memcpy(&cpu->memory[hdr.load_addr], payload, payload_size);

  /* Set initial CPU state */
  cpu->pc = hdr.exec_addr;
  cpu->sp = SOS_STACK_TOP; /* Stack at 0x0800, grows down */

  /* Push return address 0x0000 so RET triggers cold start (exit) */
  z80_push16(cpu, 0x0000);

  /* Set DTADR/SIZE/EXADR in work area */
  cpu->memory[SOS_DTADR] = hdr.load_addr & 0xFF;
  cpu->memory[SOS_DTADR + 1] = hdr.load_addr >> 8;
  cpu->memory[SOS_SIZE] = payload_size & 0xFF;
  cpu->memory[SOS_SIZE + 1] = (payload_size >> 8) & 0xFF;
  cpu->memory[SOS_EXADR] = hdr.exec_addr & 0xFF;
  cpu->memory[SOS_EXADR + 1] = hdr.exec_addr >> 8;

  return 0;
}
