/*
 * dos_host.c — MS-DOS subsystem memory image setup
 *
 * Builds the DOS memory image within a pre-allocated contiguous run.
 * The first paragraph (16 B) of the run is the process's MCB header;
 * the PSP lives in the next paragraph (proc_seg:0000 = base_linear+16)
 * and the program image follows at proc_seg:0100 (.COM) or at
 * load_seg:init_ip (.EXE, where load_seg = proc_seg + 0x10).
 *
 * Offsets inside the run are computed as (flat_linear - base_linear),
 * so all cross-page I/O goes through dos_write_run_bytes /
 * dos_read_run_bytes which split on the 4 KB page boundary.
 */

#include "kernel/core/subsys/msdos/dos_host.h"

#include <string.h>

#include "common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/exec/exec_args.h"
#include "kernel/core/mm/mem_region.h"

/* ── Page-splitting run-byte helpers ──────────────────────────────────── */

/* mem_region_page_{read,write} are single-page.  Initial .EXE frames,
 * relocation patches, and MCB/PSP writes may straddle a page boundary
 * once the run extends past the first 4 KB, so every write that is not
 * trivially confined to the first page goes through these helpers. */
static void dos_write_run_bytes(page_id_t base_id, uint32_t off_from_base,
                                const void *buf, uint32_t len) {
  const uint8_t *src = (const uint8_t *)buf;
  while (len > 0) {
    uint32_t pg = off_from_base / PAGE_SIZE;
    uint16_t pg_off = (uint16_t)(off_from_base % PAGE_SIZE);
    uint32_t chunk = PAGE_SIZE - pg_off;
    if (chunk > len) chunk = len;
    page_write(base_id + (page_id_t)pg, pg_off, src, (uint16_t)chunk);
    src += chunk;
    off_from_base += chunk;
    len -= chunk;
  }
}

static void dos_read_run_bytes(page_id_t base_id, uint32_t off_from_base,
                               void *buf, uint32_t len) {
  uint8_t *dst = (uint8_t *)buf;
  while (len > 0) {
    uint32_t pg = off_from_base / PAGE_SIZE;
    uint16_t pg_off = (uint16_t)(off_from_base % PAGE_SIZE);
    uint32_t chunk = PAGE_SIZE - pg_off;
    if (chunk > len) chunk = len;
    page_read(base_id + (page_id_t)pg, pg_off, dst, (uint16_t)chunk);
    dst += chunk;
    off_from_base += chunk;
    len -= chunk;
  }
}

/* ── Segment zeroing ──────────────────────────────────────────────────── */

static void dos_zero_segment(page_id_t base_id, uint32_t seg_pages) {
  for (uint32_t pg = 0; pg < seg_pages; pg++)
    page_zero(base_id + (page_id_t)pg, 0, PAGE_SIZE);
}

/* ── MCB construction ─────────────────────────────────────────────────── */

static void dos_write_mcb(page_id_t base_id, uint32_t mcb_off_in_run,
                          uint8_t sig, uint16_t owner, uint16_t size) {
  uint8_t bytes[DOS_MCB_BYTES];
  __builtin_memset(bytes, 0, sizeof(bytes));
  bytes[DOS_MCB_OFF_SIG] = sig;
  bytes[DOS_MCB_OFF_OWNER] = (uint8_t)(owner & 0xFF);
  bytes[DOS_MCB_OFF_OWNER + 1] = (uint8_t)(owner >> 8);
  bytes[DOS_MCB_OFF_SIZE] = (uint8_t)(size & 0xFF);
  bytes[DOS_MCB_OFF_SIZE + 1] = (uint8_t)(size >> 8);
  dos_write_run_bytes(base_id, mcb_off_in_run, bytes, DOS_MCB_BYTES);
}

/* ── Environment block ───────────────────────────────────────────────── */

/* Max DOS 8.3 basename (12 chars) + NUL. */
#define DOS_BASENAME_MAX 13

/* Extract filename portion of argv[0] (after last '/' or '\\'),
 * uppercase it, and copy into `out`.  Streams the path byte-by-byte
 * straight from the args page so no full-path stack buffer is needed
 * (1 KB ia16 kernel stack is the binding constraint — see
 * no_static_scratch feedback memory).  Returns bytes written
 * (excluding NUL). */
static int dos_argv0_basename_upper(const exec_args_t *args, char *out,
                                    int out_size) {
  out[0] = '\0';
  if (args->argc == 0) return 0;
  uint16_t plen = exec_args_argv_len(args, 0);
  /* First pass: find the offset of the last '/' or '\\' within argv[0]. */
  uint16_t base_idx = 0;
  for (uint16_t i = 0; i < plen; i++) {
    char c;
    if (exec_args_argv_byte(args, 0, i, &c) < 0) break;
    if (c == '/' || c == '\\') base_idx = (uint16_t)(i + 1u);
  }
  /* Second pass: copy basename, uppercase. */
  int n = 0;
  for (uint16_t i = base_idx; i < plen && n < out_size - 1; i++) {
    char c;
    if (exec_args_argv_byte(args, 0, i, &c) < 0) break;
    if (c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
    out[n++] = c;
  }
  out[n] = '\0';
  return n;
}

/* Build the environment block at the top of the run.  The block holds:
 *     NAME=VALUE\0 ...  (inherited from parent via envp)
 *     \0                (end-of-env terminator)
 *     \x01\x00          (DOS 3+ count of following strings)
 *     C:\<basename>\0   (DOS 3+ program-path string, from argv[0])
 *
 * When envp is NULL or empty, the entry section degenerates to a single
 * '\0' and the block is just the DOS 3+ program-path trailer — what
 * PPAP produces for first-line init-like exec paths that don't supply
 * env.
 *
 * Also writes the env MCB header (sig 'Z') at the paragraph immediately
 * below env_seg.  Env size is sized exactly to the payload — no fixed
 * overhead.  Writes go directly to the run (no staging buffer): the run
 * was already zeroed by dos_zero_segment, so only the non-zero bytes
 * need to be set (the inter-entry and trailing NULs stay as-is).
 *
 * Out-params:
 *   *out_env_seg  = segment for PSP[0x2C].
 *   *out_main_para = paragraphs available to the main MCB / mem_top
 *                    (total_para - main_mcb(1) - env_mcb(1) - env_para). */
static void dos_build_env(page_id_t base_id, uint16_t proc_seg,
                          uint32_t seg_pages, const exec_args_t *args,
                          uint16_t *out_env_seg, uint16_t *out_main_para) {
  char name[DOS_BASENAME_MAX];
  int nlen = dos_argv0_basename_upper(args, name, sizeof(name));

  /* Env payload (bytes):
   *   env_bytes = Σ (envp[i] length + 1)     (entries with NUL each)
   *   1          end-of-env terminator '\0'
   *   2          count word 0x0001
   *   3          "C:\\"
   *   nlen       basename
   *   1          trailing '\0'
   * Always at least one paragraph so a valid env MCB exists even when
   * argv[0] is missing and envp is empty. */
  uint16_t env_bytes = 0;
  for (int i = 0; i < (int)args->envc; i++)
    env_bytes += (uint16_t)(exec_args_envp_len(args, i) + 1u);
  uint16_t env_payload_bytes =
      (uint16_t)(env_bytes + 1u + 2u + 3u + (uint16_t)nlen + 1u);
  uint16_t env_para = (uint16_t)((env_payload_bytes + 15u) >> 4);
  if (env_para < 1u) env_para = 1u;

  uint32_t total_para = seg_pages * (PAGE_SIZE / 16u);
  uint32_t env_mcb_para = total_para - 1u - (uint32_t)env_para;
  uint16_t env_seg = (uint16_t)(proc_seg - 1u + env_mcb_para + 1u);
  uint32_t env_mcb_off = env_mcb_para * 16u;
  uint32_t env_payload_off = env_mcb_off + DOS_MCB_BYTES;

  dos_write_mcb(base_id, env_mcb_off, DOS_MCB_SIG_Z, proc_seg, env_para);

  /* Write each env entry directly from args page → DOS run, no
   * kernel staging buffer (see no_static_scratch feedback). */
  uint32_t pos = env_payload_off;
  for (int i = 0; i < (int)args->envc; i++) {
    uint16_t slen = exec_args_envp_len(args, i);
    if (slen) exec_args_envp_to_page(args, i, base_id, pos);
    pos += slen + 1u; /* +1 for inter-entry '\0' from zero-fill */
  }
  pos += 1u; /* end-of-env '\0' from zero-fill */
  uint8_t count[2] = {0x01, 0x00};
  dos_write_run_bytes(base_id, pos, count, 2);
  pos += 2u;
  uint8_t prefix[3] = {'C', ':', '\\'};
  dos_write_run_bytes(base_id, pos, prefix, 3);
  pos += 3u;
  if (nlen > 0)
    dos_write_run_bytes(base_id, pos, (const uint8_t *)name, (uint32_t)nlen);
  /* trailing '\0' from zero-fill */

  *out_env_seg = env_seg;
  *out_main_para = (uint16_t)(total_para - 2u - (uint32_t)env_para);
}

/* ── PSP construction ─────────────────────────────────────────────────── */

/* PSP starts one paragraph into the run (immediately after the MCB).
 * All PSP offsets below are relative to the PSP base, translated to
 * run-base offsets by adding DOS_MCB_BYTES.
 *
 * Caller must have already placed the env block (dos_build_env) so we
 * can wire PSP[0x2C] and set PSP[0x02] mem_top to the matching main
 * payload size. */
static void dos_build_psp(page_id_t base_id, uint16_t proc_seg,
                          const exec_args_t *args, uint16_t env_seg,
                          uint16_t main_para) {
  uint32_t psp_off = DOS_MCB_BYTES;

  /* PSP:0x00 — INT 20h (terminate) */
  uint8_t int20[2] = {0xCD, 0x20};
  dos_write_run_bytes(base_id, psp_off + 0x00, int20, 2);

  /* PSP:0x02 — memory top: proc_seg + main_para (end of the main block;
   * env MCB header sits one paragraph higher). */
  uint16_t mem_top = (uint16_t)(proc_seg + main_para);
  uint8_t mem_top_bytes[2] = {(uint8_t)(mem_top & 0xFF),
                              (uint8_t)(mem_top >> 8)};
  dos_write_run_bytes(base_id, psp_off + 0x02, mem_top_bytes, 2);

  /* PSP:0x2C — environment block segment. */
  uint8_t env_bytes[2] = {(uint8_t)(env_seg & 0xFF), (uint8_t)(env_seg >> 8)};
  dos_write_run_bytes(base_id, psp_off + 0x2Cu, env_bytes, 2);

  /* PSP:0x50 — INT 21h + RETF (alternate DOS entry) */
  uint8_t dos_entry[3] = {0xCD, 0x21, 0xCB};
  dos_write_run_bytes(base_id, psp_off + 0x50, dos_entry, 3);

  /* PSP:0x80 — command tail (length byte, args with leading space, CR).
   * Stream argv[1..] byte-by-byte from the args page directly into the
   * DOS run; the run was already zeroed, so we only write non-zero
   * bytes.  Total tail capacity is 126 bytes (DOS PSP layout). */
  uint8_t tail_len = 0;
  if (args->argc > 0) {
    int pos = 0;
    for (int i = 1; i < (int)args->argc && pos < 126; i++) {
      uint8_t sp = ' ';
      dos_write_run_bytes(base_id, psp_off + 0x81 + (uint32_t)pos, &sp, 1);
      pos++;
      if (pos >= 126) break;
      uint16_t alen = exec_args_argv_len(args, i);
      if (pos + (int)alen > 126) alen = (uint16_t)(126 - pos);
      for (uint16_t k = 0; k < alen; k++) {
        char c;
        if (exec_args_argv_byte(args, i, k, &c) < 0) break;
        dos_write_run_bytes(base_id, psp_off + 0x81 + (uint32_t)(pos + k),
                            (const uint8_t *)&c, 1);
      }
      pos += alen;
    }
    tail_len = (uint8_t)pos;
    uint8_t cr = '\r';
    dos_write_run_bytes(base_id, psp_off + 0x81 + (uint32_t)pos, &cr, 1);
  }
  dos_write_run_bytes(base_id, psp_off + 0x80, &tail_len, 1);
}

/* ── Binary load (.COM) ───────────────────────────────────────────────── */

/* Stream `n` bytes from `vn` at `file_off` into the process run at byte
 * offset `run_off` (from base_id:0), chunked into ≤PAGE_SIZE reads per
 * the VFS contract (memory.md §9).  A single vnode_read with
 * n > PAGE_SIZE would truncate via size_t on ia16. */
static int dos_read_into_run(page_id_t base_id, uint32_t run_off, vnode_t *vn,
                             uint32_t file_off, uint32_t n) {
  while (n > 0) {
    page_id_t pg = (page_id_t)(base_id + (page_id_t)(run_off / PAGE_SIZE));
    uint16_t pg_off = (uint16_t)(run_off % PAGE_SIZE);
    uint32_t chunk = PAGE_SIZE - pg_off;
    if (chunk > n) chunk = n;
    long r = mod_vfs.vnode_read(vn, pg, pg_off, chunk, file_off);
    if (r < 0) return (int)r;
    if ((uint32_t)r != chunk) return -(int)ENOEXEC;
    run_off += chunk;
    file_off += chunk;
    n -= chunk;
  }
  return 0;
}

static int dos_load_binary(page_id_t base_id, vnode_t *vn, uint32_t file_size) {
  /* .COM image is loaded at proc_seg:0x0100, which is offset
   * (DOS_MCB_BYTES + 0x100) from the start of the run. */
  return dos_read_into_run(base_id, DOS_MCB_BYTES + 0x100u, vn, 0, file_size);
}

/* ── Initial user-mode frame (.COM) ───────────────────────────────────── */

static uint16_t dos_build_user_frame(page_id_t base_id, uint32_t base_linear,
                                     uint16_t proc_seg) {
  /* .COM entry state:
   *   CS=DS=ES=SS=proc_seg, IP=0x0100, SP=0xFFFE, FLAGS IF=1
   *
   * Frame layout (matches trap.S restore path):
   *   HW frame (6B): IP, CS, FLAGS    (popped by IRET)
   *   SW frame (18B): ES, DS, BP, DI, SI, DX, CX, BX, AX
   */
  uint16_t user_sp_top = 0xFFFE;

  uint16_t hw_frame[3];
  hw_frame[0] = 0x0100;   /* IP */
  hw_frame[1] = proc_seg; /* CS */
  hw_frame[2] = 0x0200;   /* FLAGS: IF=1 */

  uint16_t sw_frame[9];
  sw_frame[0] = proc_seg; /* ES */
  sw_frame[1] = proc_seg; /* DS */
  sw_frame[2] = 0;        /* BP */
  sw_frame[3] = 0;        /* DI */
  sw_frame[4] = 0;        /* SI */
  sw_frame[5] = 0;        /* DX */
  sw_frame[6] = 0;        /* CX */
  sw_frame[7] = 0;        /* BX */
  sw_frame[8] = 0;        /* AX */

  uint32_t hw_flat =
      ((uint32_t)proc_seg << 4) + (user_sp_top - sizeof(hw_frame));
  uint32_t sw_flat = hw_flat - sizeof(sw_frame);

  dos_write_run_bytes(base_id, hw_flat - base_linear, hw_frame,
                      sizeof(hw_frame));
  dos_write_run_bytes(base_id, sw_flat - base_linear, sw_frame,
                      sizeof(sw_frame));

  return (uint16_t)(user_sp_top - sizeof(hw_frame) - sizeof(sw_frame));
}

/* ── Main entry point (.COM) ──────────────────────────────────────────── */

int dos_build_com_image(page_id_t base_id, uint16_t proc_seg,
                        uint32_t seg_pages, vnode_t *vn, uint32_t file_size,
                        const struct exec_args *args, uint16_t *out_user_sp) {
  if (seg_pages < DOS_SEG_PAGES) seg_pages = DOS_SEG_PAGES;

  uint32_t base_linear = page_linear(base_id);

  dos_zero_segment(base_id, seg_pages);

  /* Place env MCB + env content first so we know the exact paragraphs
   * left for the main block.  Then write the main MCB with the 'M'
   * signature (chain continues into env) and its matching payload. */
  uint16_t env_seg;
  uint16_t main_para;
  dos_build_env(base_id, proc_seg, seg_pages, args, &env_seg, &main_para);
  dos_write_mcb(base_id, 0, DOS_MCB_SIG_M, proc_seg, main_para);

  dos_build_psp(base_id, proc_seg, args, env_seg, main_para);
  int rc = dos_load_binary(base_id, vn, file_size);
  if (rc < 0) return rc;
  *out_user_sp = dos_build_user_frame(base_id, base_linear, proc_seg);
  return 0;
}

/* ── EXE (MZ) relocation handling ─────────────────────────────────────── */

/* Patch the 16-bit word at (load_seg + r_seg):r_off by adding load_seg. */
static int dos_apply_one_reloc(page_id_t base_id, uint32_t base_linear,
                               uint32_t seg_pages, uint16_t load_seg,
                               uint16_t r_off, uint16_t r_seg) {
  uint32_t target_flat = ((uint32_t)(r_seg + load_seg) << 4) + r_off;
  uint32_t run_end = base_linear + seg_pages * PAGE_SIZE;
  if (target_flat < base_linear || target_flat + 2u > run_end)
    return -(int)ENOEXEC;

  uint32_t off_from_base = target_flat - base_linear;
  uint8_t word[2];
  dos_read_run_bytes(base_id, off_from_base, word, 2);
  uint16_t val = (uint16_t)word[0] | ((uint16_t)word[1] << 8);
  val = (uint16_t)(val + load_seg);
  word[0] = (uint8_t)val;
  word[1] = (uint8_t)(val >> 8);
  dos_write_run_bytes(base_id, off_from_base, word, 2);
  return 0;
}

/* Apply all MZ relocations.  Reloc entries are staged into the PSP
 * scratch area (base_id:DOS_MCB_BYTES .. DOS_MCB_BYTES+0x100) so this
 * must run before dos_build_psp rewrites that range. */
#define DOS_RELOC_BATCH 64u /* 64 * 4 = 256 bytes fits in PSP scratch */

static int dos_apply_relocations(page_id_t base_id, uint32_t base_linear,
                                 uint32_t seg_pages, uint16_t load_seg,
                                 vnode_t *vn, uint32_t file_size,
                                 const mz_header_t *hdr) {
  if (hdr->reloc_count == 0) return 0;

  uint32_t table_bytes = (uint32_t)hdr->reloc_count * 4u;
  if ((uint32_t)hdr->reloc_offset + table_bytes > file_size)
    return -(int)ENOEXEC;

  uint32_t remaining = hdr->reloc_count;
  uint32_t file_off = hdr->reloc_offset;

  while (remaining > 0) {
    uint32_t batch =
        (remaining > DOS_RELOC_BATCH) ? DOS_RELOC_BATCH : remaining;
    uint32_t batch_bytes = batch * 4u;

    /* Stage into the PSP scratch region (base_id:DOS_MCB_BYTES).  The
     * MCB is already written above, so we keep offset 0..0xF untouched. */
    long rn = mod_vfs.vnode_read(vn, base_id, (uint16_t)DOS_MCB_BYTES,
                                 batch_bytes, file_off);
    if (rn < 0) return (int)rn;
    if ((uint32_t)rn != batch_bytes) return -(int)ENOEXEC;

    for (uint32_t i = 0; i < batch; i++) {
      uint8_t entry[4];
      page_read(base_id, (uint16_t)(DOS_MCB_BYTES + i * 4u), entry, 4);
      uint16_t r_off = (uint16_t)entry[0] | ((uint16_t)entry[1] << 8);
      uint16_t r_seg = (uint16_t)entry[2] | ((uint16_t)entry[3] << 8);

      int rc = dos_apply_one_reloc(base_id, base_linear, seg_pages, load_seg,
                                   r_off, r_seg);
      if (rc < 0) return rc;
    }

    remaining -= batch;
    file_off += batch_bytes;
  }
  return 0;
}

/* ── Main entry point (.EXE) ──────────────────────────────────────────── */

int dos_build_exe_image(page_id_t base_id, uint16_t proc_seg,
                        uint32_t seg_pages, vnode_t *vn, uint32_t file_size,
                        const mz_header_t *hdr, const struct exec_args *args,
                        uint16_t *out_user_ss, uint16_t *out_user_sp) {
  if (seg_pages < DOS_SEG_PAGES) seg_pages = DOS_SEG_PAGES;

  uint32_t base_linear = page_linear(base_id);

  /* Size math from the header. */
  uint32_t header_bytes = (uint32_t)hdr->header_size * 16u;
  if (hdr->header_size < 2u) return -(int)ENOEXEC;
  if (hdr->page_count == 0) return -(int)ENOEXEC;
  if (header_bytes > file_size) return -(int)ENOEXEC;

  uint32_t image_bytes = (uint32_t)hdr->page_count * 512u;
  if (hdr->last_page_size != 0)
    image_bytes -= (512u - (uint32_t)hdr->last_page_size);
  if (image_bytes < header_bytes) return -(int)ENOEXEC;
  uint32_t code_size = image_bytes - header_bytes;
  if (code_size > file_size - header_bytes) return -(int)ENOEXEC;

  /* Required bytes: MCB + PSP + code + min_alloc headroom. */
  uint32_t tail_para = (uint32_t)hdr->min_alloc;
  uint32_t total_bytes = DOS_MCB_BYTES + 0x100u + code_size + tail_para * 16u;
  if (total_bytes > seg_pages * PAGE_SIZE) return -(int)ENOMEM;

  /* Zero the whole run up front. */
  dos_zero_segment(base_id, seg_pages);

  /* Place env block (sizes itself) and main MCB before streaming the
   * image so MCB chain walkers from the guest see a valid chain from
   * the first instruction. */
  uint16_t env_seg;
  uint16_t main_para;
  dos_build_env(base_id, proc_seg, seg_pages, args, &env_seg, &main_para);
  dos_write_mcb(base_id, 0, DOS_MCB_SIG_M, proc_seg, main_para);

  /* Stream the image payload into proc_seg:0100 = base_id:0x110.  PSP
   * build is deferred until after relocations so the PSP scratch area
   * can stage reloc entries. */
  if (code_size > 0) {
    int sr = dos_read_into_run(base_id, DOS_MCB_BYTES + 0x100u, vn,
                               header_bytes, code_size);
    if (sr < 0) return sr;
  }

  /* Resolve initial CS:IP / SS:SP against load_seg. */
  uint16_t load_seg = (uint16_t)(proc_seg + 0x10u);

  int rr = dos_apply_relocations(base_id, base_linear, seg_pages, load_seg, vn,
                                 file_size, hdr);
  if (rr < 0) return rr;

  /* PSP at proc_seg:0 — writes over the reloc scratch area. */
  dos_build_psp(base_id, proc_seg, args, env_seg, main_para);
  uint16_t user_cs = (uint16_t)(hdr->init_cs + load_seg);
  uint16_t user_ss = (uint16_t)(hdr->init_ss + load_seg);
  uint16_t user_ip = hdr->init_ip;
  uint16_t user_sp = hdr->init_sp;

  /* Frame layout (matches trap.S restore path):
   *   HW frame (6B): IP, CS, FLAGS
   *   SW frame (18B): ES, DS, BP, DI, SI, DX, CX, BX, AX */
  uint16_t hw_frame[3];
  hw_frame[0] = user_ip;
  hw_frame[1] = user_cs;
  hw_frame[2] = 0x0200; /* IF=1 */

  uint16_t sw_frame[9];
  sw_frame[0] = proc_seg; /* ES = PSP seg */
  sw_frame[1] = proc_seg; /* DS = PSP seg */
  sw_frame[2] = 0;        /* BP */
  sw_frame[3] = 0;        /* DI */
  sw_frame[4] = 0;        /* SI */
  sw_frame[5] = 0;        /* DX */
  sw_frame[6] = 0;        /* CX */
  sw_frame[7] = 0;        /* BX */
  sw_frame[8] = 0;        /* AX */

  uint32_t hw_flat =
      ((uint32_t)user_ss << 4) + (uint32_t)user_sp - sizeof(hw_frame);
  uint32_t sw_flat = hw_flat - sizeof(sw_frame);
  uint32_t run_end = base_linear + seg_pages * PAGE_SIZE;
  if (sw_flat < base_linear || hw_flat + sizeof(hw_frame) > run_end)
    return -(int)ENOEXEC;

  dos_write_run_bytes(base_id, hw_flat - base_linear, hw_frame,
                      sizeof(hw_frame));
  dos_write_run_bytes(base_id, sw_flat - base_linear, sw_frame,
                      sizeof(sw_frame));

  *out_user_ss = user_ss;
  *out_user_sp = (uint16_t)(user_sp - sizeof(hw_frame) - sizeof(sw_frame));
  return 0;
}
