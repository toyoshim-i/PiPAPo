/*
 * x_loader.c — Human68k X-format binary loader
 *
 * Detects X-format (.x) binaries by the "HU" magic and loads them.
 * On native m68k, the binary runs directly in user mode with F-line
 * exceptions intercepted by the kernel for DOS call translation.
 * On other architectures, the binary runs via the m68k emulator.
 *
 * See docs/subsystems/human68k.md for the full design.
 */

#include <string.h>

#include "exec.h"
#include "kernel/core/endian.h"
#include "common/errno.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/subsys/subsys.h"
#include "loader.h"
#if !defined(__m68k__)
#include "h68k_emu.h"
#else
#include "kernel/core/signal/signal.h"
#include "kernel/core/subsys/human68k_loader.h"
#endif

/* ── X-format constants ────────────────────────────────────────────────── */

/* X-format magic: "HU" (0x48 0x55) at offset 0 */
#define X68K_MAGIC_0 0x48
#define X68K_MAGIC_1 0x55

/* X-format header size */
#define X68K_HEADER_SIZE 64

#if !defined(__m68k__)
/* PMB size on native is in h68k_emu.h; define here for emulated path too */
#else
#define X68K_PMB_SIZE 0x100
#endif

/*
 * X-format executable header (64 bytes, big-endian).
 */
typedef struct __attribute__((packed)) {
  uint8_t magic[2];       /* 0x00: "HU" (0x48 0x55)               */
  uint8_t load_mode;      /* 0x02: reserved (load mode)            */
  uint8_t load_addr_flag; /* 0x03: reserved (load address flag)    */
  uint32_t base_addr;     /* 0x04: base address (usually 0)        */
  uint32_t entry_offset;  /* 0x08: entry point offset from base    */
  uint32_t text_size;     /* 0x0C: text segment size               */
  uint32_t data_size;     /* 0x10: data segment size               */
  uint32_t bss_size;      /* 0x14: BSS segment size                */
  uint32_t reloc_size;    /* 0x18: relocation table size           */
  uint32_t sym_size;      /* 0x1C: symbol table size               */
  uint32_t scd_line_size; /* 0x20: SCD line number table (debug)   */
  uint32_t scd_sym_size;  /* 0x24: SCD symbol table (debug)        */
  uint32_t scd_str_size;  /* 0x28: SCD string table (debug)        */
  uint32_t reserved[4];   /* 0x2C–0x3B: reserved (0)               */
  uint32_t bind_list;     /* 0x3C: bind list address (0 if none)   */
} x68k_header_t;

_Static_assert(sizeof(x68k_header_t) == X68K_HEADER_SIZE,
               "x68k_header_t must be exactly 64 bytes");

/* ── Detection ─────────────────────────────────────────────────────────── */

static int x_detect(const uint8_t *file_buf, uint32_t file_size,
                    const char *path) {
  (void)path;
  if (file_size < X68K_HEADER_SIZE) return 0;
  return file_buf[0] == X68K_MAGIC_0 && file_buf[1] == X68K_MAGIC_1;
}

/* ── Header validation ─────────────────────────────────────────────────── */

static int x68k_validate(const x68k_header_t *hdr, uint32_t file_size) {
  uint32_t entry_offset = be32_load(&hdr->entry_offset);
  uint32_t text_size = be32_load(&hdr->text_size);
  uint32_t data_size = be32_load(&hdr->data_size);
  uint32_t bss_size = be32_load(&hdr->bss_size);
  uint32_t reloc_size = be32_load(&hdr->reloc_size);

  if (hdr->magic[0] != X68K_MAGIC_0 || hdr->magic[1] != X68K_MAGIC_1)
    return -(int)ENOEXEC;

  if (entry_offset >= text_size && text_size > 0) return -(int)ENOEXEC;

  uint32_t needed = X68K_HEADER_SIZE + text_size + data_size + reloc_size;
  if (needed > file_size) return -(int)ENOEXEC;

  uint32_t image_size = text_size + data_size + bss_size;
  if (image_size < text_size) return -(int)ENOEXEC;

  if (text_size == 0) return -(int)ENOEXEC;

  return 0;
}

/* ── Relocation processor ──────────────────────────────────────────────── */

static int x68k_apply_relocs(uint8_t *image, uint32_t image_size,
                             const uint8_t *reloc_table, uint32_t reloc_size,
                             uint32_t delta) {
  if (reloc_size == 0 || delta == 0) return 0;

  uint32_t pos = 0;
  uint32_t fixup = 0;
  int first = 1;

  while (pos < reloc_size) {
    uint32_t disp;

    if (pos + 2 > reloc_size) break;

    uint16_t d16 = be16_load(reloc_table + pos);
    pos += 2;

    if (first) {
      fixup = d16;
      first = 0;
    } else {
      if (d16 == 0x0001) {
        if (pos + 4 > reloc_size) return -(int)ENOEXEC;
        disp = be32_load(reloc_table + pos);
        pos += 4;
        fixup += disp;
      } else {
        fixup += d16;
      }
    }

    if (fixup & 1) {
      uint32_t off = fixup & ~1u;
      if (off + 2 > image_size) return -(int)ENOEXEC;
      uint16_t val = be16_load(image + off);
      be16_store(image + off, (uint16_t)(val + delta));
    } else {
      if (fixup + 4 > image_size) return -(int)ENOEXEC;
      uint32_t val = be32_load(image + fixup);
      be32_store(image + fixup, val + delta);
    }
  }

  return 0;
}

static int x68k_alloc_largest_image_region(proc_image_segment_t *seg,
                                           uint32_t min_pages) {
  if (!seg || min_pages == 0 || min_pages > USER_PAGES_MAX)
    return -(int)ENOMEM;

  for (uint32_t total_pages = USER_PAGES_MAX; total_pages >= min_pages;
       total_pages--) {
    if (mem_region_alloc(seg, PPAP_MEM_RAM_DATA, total_pages * PAGE_SIZE,
                         PROC_IMAGE_SEG_WRITABLE) == 0)
      return (int)total_pages;
    if (total_pages == min_pages) break;
  }

  *seg = (proc_image_segment_t){0};
  return -(int)ENOMEM;
}

/* ── Loader ────────────────────────────────────────────────────────────── */

static int x_load(pcb_t *p, const uint8_t *file_buf, uint32_t file_size,
                  const cpu_ops_t *cpu_ops, void *cpu_state,
                  const char *const *argv, uint32_t flags) {
  (void)flags;
  (void)cpu_ops;
  (void)cpu_state;

  const x68k_header_t *hdr = (const x68k_header_t *)file_buf;
  int err = x68k_validate(hdr, file_size);
  if (err < 0) return err;

  uint32_t entry_offset = be32_load(&hdr->entry_offset);
  uint32_t text_size = be32_load(&hdr->text_size);
  uint32_t data_size = be32_load(&hdr->data_size);
  uint32_t bss_size = be32_load(&hdr->bss_size);
  uint32_t reloc_size = be32_load(&hdr->reloc_size);
  uint32_t base_addr = be32_load(&hdr->base_addr);
  uint32_t image_size = text_size + data_size + bss_size;
  proc_image_segment_t stack_region = {0};
  proc_image_segment_t image_region = {0};

  if (mem_region_alloc(&stack_region, PPAP_MEM_RAM_STACK, PAGE_SIZE,
                       PROC_IMAGE_SEG_WRITABLE |
                           PROC_IMAGE_SEG_OWNED) < 0) {
    return -(int)ENOMEM;
  }
  p->stack_page_id = mem_region_ptr_to_page(stack_region.base);
  p->image.stack = stack_region;

#if !defined(__m68k__) && defined(PPAP_ENABLE_ECPU_M68K)
  /* ── Emulated path (requires m68k eCPU) ─────────────────────────────── */

  uint32_t min_pages =
      (X68K_PMB_SIZE + image_size + PAGE_SIZE - 1u) / PAGE_SIZE;
  if (min_pages > H68K_EMU_MEM_PAGES_MAX) {
    mem_region_free(&stack_region);
    p->stack_page_id = PAGE_ID_INVALID;
    p->image.stack = (proc_image_segment_t){0};
    return -(int)ENOMEM;
  }

  uint32_t min_total_pages = min_pages + 1u; /* +1 page for emu state */
  int total_pages = x68k_alloc_largest_image_region(&image_region,
                                                    min_total_pages);
  if (total_pages < 0) {
    mem_region_free(&stack_region);
    p->stack_page_id = PAGE_ID_INVALID;
    p->image.stack = (proc_image_segment_t){0};
    return total_pages;
  }

  if (proc_track_page_range(p, 0, mem_region_ptr_to_page(image_region.base),
                            image_region.size / PAGE_SIZE) < 0) {
    mem_region_free(&image_region);
    mem_region_free(&stack_region);
    p->stack_page_id = PAGE_ID_INVALID;
    p->image.stack = (proc_image_segment_t){0};
    return -(int)ENOMEM;
  }

  uint8_t *mem_base = (uint8_t *)image_region.base;
  uint32_t emu_mem_pages = (uint32_t)total_pages - 1u;
  uint32_t emu_mem_size = emu_mem_pages * PAGE_SIZE;

  uint8_t *emu_mem = mem_base;
  h68k_emu_exec_state_t *st =
      (h68k_emu_exec_state_t *)(mem_base + emu_mem_pages * PAGE_SIZE);
  memset(st, 0, sizeof(*st));
  ecpu_m68k_ops.init((cpu_state_t *)&st->m68k, emu_mem, emu_mem_size);
  ecpu_m68k_ops.set_trap_handler((cpu_state_t *)&st->m68k,
                                 h68k_emu_trap_handler, st);

  /* PMB + text/data/bss at guest addresses 0x0000.. */
  memset(emu_mem, 0, emu_mem_size);
  {
    uint32_t guest_end = emu_mem_size;
    be32_store(emu_mem + 0x00, 0);
    be32_store(emu_mem + 0x04, 0);
    be32_store(emu_mem + 0x08, guest_end);
    be32_store(emu_mem + 0x0C, 0);
    be32_store(emu_mem + 0x10, 0xFFFFFFFFu);
    be32_store(emu_mem + 0x20, 0x0000006Cu);
    emu_mem[0x24] = 0x07;
    be32_store(emu_mem + 0x30, X68K_PMB_SIZE + image_size);
    be32_store(emu_mem + 0x34, X68K_PMB_SIZE + image_size);
    be32_store(emu_mem + 0x38, guest_end);
  }
  st->heap_next = (X68K_PMB_SIZE + image_size + 3u) & ~3u;
  st->block_end = emu_mem_size;

  memcpy(emu_mem + X68K_PMB_SIZE, file_buf + X68K_HEADER_SIZE, text_size);
  if (data_size > 0)
    memcpy(emu_mem + X68K_PMB_SIZE + text_size,
           file_buf + X68K_HEADER_SIZE + text_size, data_size);
  if (bss_size > 0)
    memset(emu_mem + X68K_PMB_SIZE + text_size + data_size, 0, bss_size);

  if (reloc_size > 0) {
    uint32_t delta = X68K_PMB_SIZE - base_addr;
    const uint8_t *reloc_data =
        file_buf + X68K_HEADER_SIZE + text_size + data_size;
    err = x68k_apply_relocs(emu_mem + X68K_PMB_SIZE, image_size, reloc_data,
                            reloc_size, delta);
    if (err < 0) {
      proc_release_tracked_pages(p, 0, (uint32_t)total_pages);
      mem_region_free(&stack_region);
      p->stack_page_id = PAGE_ID_INVALID;
      p->image.stack = (proc_image_segment_t){0};
      return err;
    }
  }

  st->m68k.pc = X68K_PMB_SIZE + entry_offset;
  st->m68k.a[0] = 0x00000000u;
  st->m68k.a[1] = emu_mem_size;
  st->m68k.a[2] = 0x0000006Cu;
  st->m68k.a[3] = 0xFFFFFFFFu;
  st->m68k.a[4] = X68K_PMB_SIZE;
  st->m68k.a[7] = emu_mem_size;

  p->image.text = proc_image_segment_make(emu_mem + X68K_PMB_SIZE, text_size,
                                          PPAP_MEM_RAM_TEXT,
                                          PROC_IMAGE_SEG_EXECUTABLE);
  p->image.data = image_region;
  p->image.entry = X68K_PMB_SIZE + entry_offset;
  p->subsys = SUBSYS_PPAP;
  p->subsys_data = st;
  proc_setup_stack(p, h68k_emu_run_process, 0);

  (void)argv;
  return 0;

#else
  /* ── Native m68k path ──────────────────────────────────────────────── */

  uint32_t min_bytes = X68K_PMB_SIZE + image_size;
  uint32_t min_pages = (min_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

  if (min_pages > USER_PAGES_MAX) {
    mem_region_free(&stack_region);
    p->stack_page_id = PAGE_ID_INVALID;
    p->image.stack = (proc_image_segment_t){0};
    return -(int)ENOMEM;
  }
  int n_pages = x68k_alloc_largest_image_region(&image_region, min_pages);
  if (n_pages < 0) {
    mem_region_free(&stack_region);
    p->stack_page_id = PAGE_ID_INVALID;
    p->image.stack = (proc_image_segment_t){0};
    return n_pages;
  }

  if (proc_track_page_range(p, 0, mem_region_ptr_to_page(image_region.base),
                            image_region.size / PAGE_SIZE) < 0) {
    mem_region_free(&image_region);
    mem_region_free(&stack_region);
    p->stack_page_id = PAGE_ID_INVALID;
    p->image.stack = (proc_image_segment_t){0};
    return -(int)ENOMEM;
  }

  uint8_t *base = (uint8_t *)image_region.base;
  uint32_t total_bytes = (uint32_t)n_pages * PAGE_SIZE;

  /* Zero PMB, copy text+data, zero BSS */
  memset(base, 0, X68K_PMB_SIZE);

  uint8_t *text_dst = base + X68K_PMB_SIZE;
  const char *path = (argv && argv[0]) ? argv[0] : "";

  x68k_setup_pmb(base, total_bytes, image_size, path);

  const uint8_t *text_src = file_buf + X68K_HEADER_SIZE;
  memcpy(text_dst, text_src, text_size);

  if (data_size > 0)
    memcpy(text_dst + text_size, text_src + text_size, data_size);

  if (bss_size > 0) memset(text_dst + text_size + data_size, 0, bss_size);

  /* Apply relocations */
  if (reloc_size > 0) {
    uint32_t load_addr = (uint32_t)(uintptr_t)text_dst;
    uint32_t delta = load_addr - base_addr;
    const uint8_t *reloc_data =
        file_buf + X68K_HEADER_SIZE + text_size + data_size;
    err =
        x68k_apply_relocs(text_dst, image_size, reloc_data, reloc_size, delta);
    if (err < 0) {
      proc_release_tracked_pages(p, 0, (uint32_t)n_pages);
      mem_region_free(&stack_region);
      p->stack_page_id = PAGE_ID_INVALID;
      p->image.stack = (proc_image_segment_t){0};
      return err;
    }
  }

  /* Set up entry point and stack frame */
  uint32_t entry = (uint32_t)(uintptr_t)(text_dst + entry_offset);
  proc_setup_stack(p, (void (*)(void))(uintptr_t)entry, 0);
  p->image.text = proc_image_segment_make(text_dst, text_size,
                                          PPAP_MEM_RAM_TEXT,
                                          PROC_IMAGE_SEG_EXECUTABLE);
  p->image.data = image_region;
  p->image.entry = entry;

#if defined(__m68k__)
  p->usp = (uint32_t)(uintptr_t)(base + total_bytes);
#endif

  x68k_setup_registers(p->sp, (uint32_t)(uintptr_t)base,
                       (uint32_t)(uintptr_t)(base + total_bytes),
                       (uint32_t)(uintptr_t)(base + 0x6C),
                       (uint32_t)(uintptr_t)text_dst);

  p->subsys = SUBSYS_HUMAN68K;
  {
    const subsys_ops_t *ops = subsys_ops_table[SUBSYS_HUMAN68K];
    if (ops && ops->on_init) ops->on_init(p);
  }

  (void)argv;
  (void)file_size;
  return 0;
#endif
}

/* ── Loader registration ───────────────────────────────────────────────── */

const loader_t x_loader = {
    .name = "x68k",
    .detect = x_detect,
    .load = x_load,
    .required_arch_id = CPU_ARCH_M68K,
    .xip = 0,
};
