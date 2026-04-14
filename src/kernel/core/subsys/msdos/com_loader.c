/*
 * com_loader.c --- MS-DOS .COM binary loader
 *
 * Loads DOS .COM flat binaries into a 64 KB segment with a PSP at
 * offset 0x0000 and the program at 0x0100.  CS=DS=ES=SS=segment.
 *
 * On native i16 the program runs directly; INT 21h is trapped by
 * the dos_trap.S ISR.  On other architectures the i8086 eCPU would
 * be used (not yet implemented).
 */

#include "kernel/core/subsys/msdos/com_loader.h"

#include <string.h>

#include "kernel/common/core/subsys_info.h"
#include "kernel/core/cpu/cpu.h"
#include "kernel/core/exec/loader.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/subsys/msdos/dos_bridge.h"
#include "kernel/core/subsys/subsys.h"

#define COM_SEG_PAGES 16u
#define COM_SEG_BYTES ((uint32_t)COM_SEG_PAGES * PAGE_SIZE)
#define COM_STACK_PAGE (COM_SEG_PAGES - 1u)
#define COM_MAX_SIZE 0xFEFFu /* 64 KB - 256 (PSP) - 1 */

/* ── Detection ─────────────────────────────────────────────────────────── */

static int dos_com_detect(const uint8_t *file_buf, uint32_t file_size,
                          const char *path) {
  (void)file_buf;

  if (file_size == 0 || file_size > COM_MAX_SIZE) return 0;

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

static int dos_com_load(pcb_t *p, const uint8_t *file_buf, uint32_t file_size,
                        const cpu_ops_t *cpu_ops, void *cpu_state,
                        const char *const *argv, uint32_t flags) {
  (void)cpu_ops;
  (void)cpu_state;
  (void)flags;

  if (file_size > COM_MAX_SIZE) return -1;

  /* 1. Allocate a 64 KB segment (16 contiguous pages) */
  page_id_t base_id = mem_region_page_alloc_contiguous(COM_SEG_PAGES);
  if (base_id == PAGE_ID_INVALID) return -1;

  /* Zero the segment */
  {
    uint8_t zeros[64];
    __builtin_memset(zeros, 0, sizeof(zeros));
    for (uint16_t pg = 0; pg < COM_SEG_PAGES; pg++)
      for (uint16_t off = 0; off < PAGE_SIZE; off += sizeof(zeros))
        mem_region_page_write(base_id + pg, off, zeros, sizeof(zeros));
  }

  uint32_t base_linear = mem_region_page_linear(base_id);
  uint16_t proc_seg = (uint16_t)(base_linear >> 4);

  /* 2. Build PSP at seg:0000 */
  {
    uint16_t int20 = 0x20CD; /* INT 20h */
    mem_region_page_write(base_id, 0x00, &int20, 2);

    uint16_t mem_top = proc_seg + 0x1000; /* top of 64 KB segment */
    mem_region_page_write(base_id, 0x02, &mem_top, 2);

    /* PSP:0x50 — INT 21h + RETF (alternate DOS entry) */
    uint8_t dos_entry[3] = {0xCD, 0x21, 0xCB};
    mem_region_page_write(base_id, 0x50, dos_entry, 3);

    /* PSP:0x80 — command tail (length + args + CR) */
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

  /* 3. Copy .COM binary at seg:0100 */
  {
    const uint8_t *src = file_buf;
    uint32_t rem = file_size;
    uint16_t dst = 0x0100;
    while (rem > 0) {
      uint16_t pg_off = dst % PAGE_SIZE;
      uint16_t chunk = PAGE_SIZE - pg_off;
      if (chunk > rem) chunk = (uint16_t)rem;
      mem_region_page_write(base_id + dst / PAGE_SIZE, pg_off, src, chunk);
      src += chunk;
      dst += chunk;
      rem -= chunk;
    }
  }

  /* 4. Tag as MS-DOS subsystem */
  p->subsys = SUBSYS_MSDOS;
  {
    const subsys_ops_t *ops = subsys_ops_table[SUBSYS_MSDOS];
    if (ops && ops->on_init) ops->on_init(p);
  }

  /* 5. Build initial user-mode frame on the user stack.
   *    .COM entry: CS=DS=ES=SS=proc_seg, IP=0x0100, SP=0xFFFE
   *    The frame layout matches trap.S restore path:
   *      HW frame (6B): IP, CS, FLAGS  (popped by IRET)
   *      SW frame (18B): ES, DS, BP, DI, SI, DX, CX, BX, AX
   */
  uint16_t user_sp_top = 0xFFFE;

  uint16_t hw_frame[3];
  hw_frame[0] = 0x0100;   /* IP = entry point */
  hw_frame[1] = proc_seg; /* CS = process segment */
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

  /* Write hardware frame at top of user stack */
  uint16_t hw_off_seg = (uint16_t)(user_sp_top - sizeof(hw_frame));
  mem_region_page_write(base_id + hw_off_seg / PAGE_SIZE,
                        hw_off_seg % PAGE_SIZE, hw_frame, sizeof(hw_frame));

  /* Write software frame below hardware frame */
  uint16_t sw_off_seg = hw_off_seg - (uint16_t)sizeof(sw_frame);
  mem_region_page_write(base_id + sw_off_seg / PAGE_SIZE,
                        sw_off_seg % PAGE_SIZE, sw_frame, sizeof(sw_frame));

  uint16_t user_sp = sw_off_seg;

  /* 6. Store user SS:SP in PCB and build kernel stack frame */
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

  /* 7. Track pages and set image metadata */
  for (uint16_t i = 0; i < COM_SEG_PAGES; i++)
    proc_track_page(p, i, base_id + i);

  p->image.text = proc_image_segment_make(
      (void *)(uintptr_t)(uint16_t)base_linear, file_size + 0x100,
      PPAP_MEM_RAM_TEXT, PROC_IMAGE_SEG_EXECUTABLE);
  p->image.text.base_page = base_id;
  p->image.data = proc_image_segment_make(
      (void *)(uintptr_t)(uint16_t)base_linear, COM_SEG_BYTES,
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
    .detect = dos_com_detect,
    .load = dos_com_load,
    .required_arch_id = CPU_ARCH_8086,
    .xip = 0,
};
