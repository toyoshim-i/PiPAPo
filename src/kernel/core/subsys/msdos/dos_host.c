/*
 * dos_host.c — MS-DOS subsystem memory image setup
 *
 * Builds the DOS memory image within a pre-allocated 64 KB segment:
 *   - Zeros the segment
 *   - Constructs a PSP (Program Segment Prefix) at offset 0
 *   - Copies the .COM binary at offset 0x0100
 *   - Constructs the initial user-mode stack frame (HW + SW) for
 *     the first IRET-like return into user code
 */

#include "kernel/core/subsys/msdos/dos_host.h"

#include <string.h>

#include "common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/mm/mem_region.h"

/* ── Segment zeroing ──────────────────────────────────────────────────── */

static void dos_zero_segment(page_id_t base_id, uint32_t seg_pages) {
  for (uint32_t pg = 0; pg < seg_pages; pg++)
    mem_region_page_zero(base_id + (page_id_t)pg, 0, PAGE_SIZE);
}

/* ── PSP construction ─────────────────────────────────────────────────── */

static void dos_build_psp(page_id_t base_id, uint16_t proc_seg,
                          uint32_t seg_pages, const char *const *argv) {
  /* PSP:0x00 — INT 20h (terminate) */
  uint16_t int20 = 0x20CD;
  mem_region_page_write(base_id, 0x00, &int20, 2);

  /* PSP:0x02 — memory top: paragraph just past the last byte the proc
   * owns.  PAGE_SIZE/16 paragraphs per page. */
  uint16_t mem_top = (uint16_t)(proc_seg + seg_pages * (PAGE_SIZE / 16u));
  mem_region_page_write(base_id, 0x02, &mem_top, 2);

  /* PSP:0x50 — INT 21h + RETF (alternate DOS entry) */
  uint8_t dos_entry[3] = {0xCD, 0x21, 0xCB};
  mem_region_page_write(base_id, 0x50, dos_entry, 3);

  /* PSP:0x80 — command tail (length byte, args with leading space, CR) */
  uint8_t tail_len = 0;
  uint8_t tail_buf[128];
  __builtin_memset(tail_buf, 0, sizeof(tail_buf));
  if (argv && argv[0]) {
    int pos = 0;
    for (int i = 1; argv[i] && pos < 126; i++) {
      if (pos < 126) tail_buf[1 + pos++] = ' ';
      int alen = (int)strlen(argv[i]);
      if (pos + alen > 126) alen = 126 - pos;
      memcpy(tail_buf + 1 + pos, argv[i], (uint16_t)alen);
      pos += alen;
    }
    tail_len = (uint8_t)pos;
    tail_buf[1 + pos] = '\r';
  }
  tail_buf[0] = tail_len;
  mem_region_page_write(base_id, 0x80, tail_buf, (uint16_t)(2 + tail_len));
}

/* ── Binary load ──────────────────────────────────────────────────────── */

static int dos_load_binary(page_id_t base_id, vnode_t *vn, uint32_t file_size) {
  /* vnode_read resolves (page, page_off) to a linear destination and
   * services the read across page boundaries, so a single call covers
   * the whole binary.  Destination is offset 0x0100 inside the DOS
   * segment, which lives entirely in page `base_id`. */
  long n = mod_vfs.vnode_read(vn, base_id, 0x0100, file_size, 0);
  if (n < 0) return (int)n;
  if ((uint32_t)n != file_size) return -(int)ENOEXEC;
  return 0;
}

/* ── Initial user-mode frame ──────────────────────────────────────────── */

static uint16_t dos_build_user_frame(page_id_t base_id, uint16_t proc_seg) {
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

  uint16_t hw_off_seg = (uint16_t)(user_sp_top - sizeof(hw_frame));
  mem_region_page_write(base_id + hw_off_seg / PAGE_SIZE,
                        hw_off_seg % PAGE_SIZE, hw_frame, sizeof(hw_frame));

  uint16_t sw_off_seg = hw_off_seg - (uint16_t)sizeof(sw_frame);
  mem_region_page_write(base_id + sw_off_seg / PAGE_SIZE,
                        sw_off_seg % PAGE_SIZE, sw_frame, sizeof(sw_frame));

  return sw_off_seg;
}

/* ── Main entry point ─────────────────────────────────────────────────── */

int dos_build_com_image(page_id_t base_id, uint16_t proc_seg,
                        uint32_t seg_pages, vnode_t *vn, uint32_t file_size,
                        const char *const *argv, uint16_t *out_user_sp) {
  if (seg_pages < DOS_SEG_PAGES) seg_pages = DOS_SEG_PAGES;
  dos_zero_segment(base_id, seg_pages);
  dos_build_psp(base_id, proc_seg, seg_pages, argv);
  int rc = dos_load_binary(base_id, vn, file_size);
  if (rc < 0) return rc;
  *out_user_sp = dos_build_user_frame(base_id, proc_seg);
  return 0;
}

/* ── EXE (MZ) image build ─────────────────────────────────────────────── */

/* Write `len` bytes into the run at `off_from_base`, splitting on page
 * boundaries.  mem_region_page_write is single-page; the initial frame
 * for .EXE can live at any SS:SP inside the run so the write may cross
 * pages. */
static void dos_write_run_bytes(page_id_t base_id, uint32_t off_from_base,
                                const void *buf, uint32_t len) {
  const uint8_t *src = (const uint8_t *)buf;
  while (len > 0) {
    uint32_t pg = off_from_base / PAGE_SIZE;
    uint16_t pg_off = (uint16_t)(off_from_base % PAGE_SIZE);
    uint32_t chunk = PAGE_SIZE - pg_off;
    if (chunk > len) chunk = len;
    mem_region_page_write(base_id + (page_id_t)pg, pg_off, src,
                          (uint16_t)chunk);
    src += chunk;
    off_from_base += chunk;
    len -= chunk;
  }
}

int dos_build_exe_image(page_id_t base_id, uint16_t proc_seg,
                        uint32_t seg_pages, vnode_t *vn, uint32_t file_size,
                        const mz_header_t *hdr, const char *const *argv,
                        uint16_t *out_user_ss, uint16_t *out_user_sp) {
  if (seg_pages < DOS_SEG_PAGES) seg_pages = DOS_SEG_PAGES;

  /* Size math from the header. */
  uint32_t header_bytes = (uint32_t)hdr->header_size * 16u;
  if (hdr->header_size < 2u) return -(int)ENOEXEC;
  if (header_bytes > file_size) return -(int)ENOEXEC;

  uint32_t image_bytes = (uint32_t)hdr->page_count * 512u;
  if (hdr->last_page_size != 0)
    image_bytes -= (512u - (uint32_t)hdr->last_page_size);
  if (image_bytes < header_bytes) return -(int)ENOEXEC;
  uint32_t code_size = image_bytes - header_bytes;
  if (code_size > file_size - header_bytes) return -(int)ENOEXEC;

  /* Required bytes: PSP + code + min_alloc headroom.  The allocation in
   * the loader is already sized against the header, but re-check here
   * against seg_pages since we write the initial frame below. */
  uint32_t tail_para = (uint32_t)hdr->min_alloc;
  uint32_t total_bytes = 0x100u + code_size + tail_para * 16u;
  if (total_bytes > seg_pages * PAGE_SIZE) return -(int)ENOMEM;

  /* Zero the whole run, then PSP at proc_seg:0. */
  dos_zero_segment(base_id, seg_pages);
  dos_build_psp(base_id, proc_seg, seg_pages, argv);

  /* Stream the image payload into base_id:0x100. */
  if (code_size > 0) {
    long n = mod_vfs.vnode_read(vn, base_id, 0x100, code_size, header_bytes);
    if (n < 0) return (int)n;
    if ((uint32_t)n != code_size) return -(int)ENOEXEC;
  }

  /* Resolve initial CS:IP / SS:SP against load_seg. */
  uint16_t load_seg = (uint16_t)(proc_seg + 0x10u);
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

  /* Flat linear addresses of the frames, relative to run base. */
  uint32_t run_base = (uint32_t)proc_seg << 4;
  uint32_t ss_base = (uint32_t)user_ss << 4;
  uint32_t hw_flat = ss_base + (uint32_t)user_sp - sizeof(hw_frame);
  uint32_t sw_flat = hw_flat - sizeof(sw_frame);

  uint32_t run_end = run_base + seg_pages * PAGE_SIZE;
  if (sw_flat < run_base || hw_flat + sizeof(hw_frame) > run_end)
    return -(int)ENOEXEC;

  dos_write_run_bytes(base_id, hw_flat - run_base, hw_frame, sizeof(hw_frame));
  dos_write_run_bytes(base_id, sw_flat - run_base, sw_frame, sizeof(sw_frame));

  *out_user_ss = user_ss;
  *out_user_sp = (uint16_t)(user_sp - sizeof(hw_frame) - sizeof(sw_frame));
  return 0;
}
