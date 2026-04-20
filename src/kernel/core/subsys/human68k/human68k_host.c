/*
 * human68k_host.c — Human68k subsystem memory image setup (PMB, registers)
 *
 * Common initialization and memory setup for Human68k X-format and R-format
 * binaries. Handles PMB (Process Memory Block) setup, register initialization,
 * and process state configuration.
 *
 * See docs/subsystems/human68k.md for the full design.
 */

#include "kernel/core/subsys/human68k/human68k_host.h"

#include <string.h>

#include "common/errno.h"
#include "kernel/core/endian.h"
#include "kernel/core/mm/mem_region.h"

/* ── Env block ─────────────────────────────────────────────────────────── */

int human68k_build_env(const char *const *envp, page_id_t *out_env_page,
                       uint32_t *out_env_addr) {
  if (!envp || !envp[0]) {
    *out_env_page = PAGE_ID_INVALID;
    *out_env_addr = 0xFFFFFFFFu;
    return 0;
  }

  page_id_t pg = mem_region_page_alloc();
  if (pg == PAGE_ID_INVALID) return -(int)ENOMEM;
  uint8_t *p = (uint8_t *)mem_region_page_to_ptr(pg);
  memset(p, 0, PAGE_SIZE);

  /* Payload starts after the 4-byte BE size header.  Fit up to
   * PAGE_SIZE - 4 bytes of entries + terminator. */
  const uint32_t payload_max = PAGE_SIZE - 4u;
  uint8_t *dst = p + 4;
  uint32_t used = 0;

  for (int i = 0; envp[i]; i++) {
    size_t len = strlen(envp[i]) + 1u;
    if (used + len + 1u > payload_max) break; /* reserve 1 B for terminator */
    memcpy(dst + used, envp[i], len);
    used += (uint32_t)len;
  }
  /* Final '\0' terminator — already zero from memset; count it. */
  used += 1u;

  be32_store(p, used);

  *out_env_page = pg;
  *out_env_addr = (uint32_t)(uintptr_t)p;
  return 0;
}

/* ── PMB setup ─────────────────────────────────────────────────────────── */

/*
 * human68k_setup_pmb — initialize Process Memory Block fields.
 *
 * Sets up the PMB structure (§4.4 MMB header + §4.5 PMB fields) at the
 * start of the allocated memory block.
 *
 * Arguments:
 *   base        — pointer to allocated memory block
 *   total_bytes — total size of allocated block
 *   image_size  — size of loaded image (text + data + bss)
 *   path        — full path to executable (for copying to path field)
 */
void human68k_setup_pmb(uint8_t *base, uint32_t total_bytes,
                        uint32_t image_size, const char *path,
                        uint32_t env_addr) {
  /* PMB offset: image starts at base+0x100 */
  uint32_t base_u = (uint32_t)(uintptr_t)base;
  uint32_t end_u = base_u + total_bytes;
  uint32_t text_u = (uint32_t)(uintptr_t)(base + 0x100);
  uint32_t bss_u = text_u + image_size;
  uint32_t heap_u = bss_u;

  /* MMB header (0x00–0x0F) */
  be32_store(base + 0x00, 0);      /* prev = 0 (first) */
  be32_store(base + 0x04, base_u); /* owner = self */
  be32_store(base + 0x08, end_u);  /* block end + 1 */
  be32_store(base + 0x0C, 0);      /* next = 0 (last) */

  /* PMB fields (0x10–0xFF).  env_addr is 0xFFFFFFFF when no parent
   * envp was supplied, matching Human68k's "no environment" marker. */
  be32_store(base + 0x10, env_addr);
  /* 0x14: exit handler — filled when F-line bridge is ready */
  be32_store(base + 0x20, base_u + 0x6C); /* cmdline (empty LASCIIZ) */
  /* 0x24: file handle bitmap — stdin/stdout/stderr open */
  base[0x24] = 0x07; /* bits 0,1,2 set = handles 0,1,2 */
  /* 0x30–0x38: segment addresses */
  be32_store(base + 0x30, bss_u);  /* BSS start */
  be32_store(base + 0x34, heap_u); /* heap start */
  be32_store(base + 0x38, end_u);  /* initial stack (top) */

  /* 0x82: execution file path (directory portion, max 65 chars + NUL) */
  if (path) {
    const char *last_slash = path;
    for (const char *s = path; *s; s++) {
      if (*s == '/') last_slash = s;
    }
    size_t dir_len = (size_t)(last_slash - path);
    if (dir_len > 65) dir_len = 65;
    if (dir_len > 0) memcpy(base + 0x82, path, dir_len);
    base[0x82 + dir_len] = '\0';

    /* 0xC4: execution file name (max 23 chars + NUL) */
    const char *bname = last_slash + 1;
    size_t name_len = strlen(bname);
    if (name_len > 23) name_len = 23;
    memcpy(base + 0xC4, bname, name_len);
    base[0xC4 + name_len] = '\0';
  }
}

/* ── Register patching ─────────────────────────────────────────────────── */

/*
 * human68k_setup_registers — patch initial CPU registers in stack frame.
 *
 * Sets up the software interrupt frame that proc_setup_stack() creates,
 * initializing the initial register values (A0–A4) according to the
 * Human68k protocol (§4.3).
 *
 * Arguments:
 *   sp          — stack pointer (kernel SSP) from p->sp
 *   pmb_base    — base address of PMB
 *   block_end   — end of allocated block (for A1 = end + 1)
 *   cmdline_ptr — address of command line buffer
 *   entry_ptr   — entry point address (text base or image start)
 */
void human68k_setup_registers(uint32_t sp, uint32_t pmb_base,
                              uint32_t block_end, uint32_t cmdline_ptr,
                              uint32_t env_addr, uint32_t entry_ptr) {
  /* Software frame (low → high from sp):
   *   [0..7]  d0–d7    [8..14] a0–a6    then SR(2)+PC(4)
   */
  uint32_t *sw = (uint32_t *)(uintptr_t)sp;
  sw[8] = pmb_base;     /* a0 = PMB */
  sw[9] = block_end;    /* a1 = end + 1 */
  sw[10] = cmdline_ptr; /* a2 = cmdline */
  sw[11] = env_addr;    /* a3 = env (0xFFFFFFFF when none) */
  sw[12] = entry_ptr;   /* a4 = entry point (text base or image start) */
}
