/*
 * target_pcxt.c — PC/XT target hooks for the PPAP kernel
 *
 * Isolated segment split.  Core + VFS modules in separate segments.
 * Reads mod_info from stage2, initializes segment manager, patches
 * far pointer tables.  Chains BIOS INT 08h for floppy motor timeout.
 */

#include "kernel/core/driver/timer_pit.h"
#include "target/target.h"
#include "kernel/common/core/seg.h"
#include "kernel/vfs/driver/blkdev.h"
#include "kernel/common/mod/mod_vfs.h"

extern void floppy_blk_init(void);

/* Page pool base: set from stage2's mod_info before mm_init(). */
uint32_t i16_page_pool_base = 0x10000ul;  /* safe default */

/* Far pointer tables from the stub assembly files */
extern uint16_t vfs_fptrs[];  /* in core: caller stubs for VFS */

/* mod_info_t — boot protocol from stage2 (at 0x0500) */
#define MOD_INFO_ADDR  0x0500u
#define MOD_MAX        4u

typedef struct {
  uint16_t count;
  struct {
    uint16_t segment;
    uint16_t size;
  } mod[MOD_MAX];
} mod_info_t;

/*
 * VFS module header layout (at offset 0 of VFS binary):
 *   [0] uint16_t magic (0x5646 = "VF")
 *   [2] uint16_t entry_count
 *   [4] uint16_t entry_offsets[entry_count]
 */
#define VFS_HDR_MAGIC 0x5646u

/* Read a 16-bit word from seg:offset using ES:BX */
static uint16_t far_read16(uint16_t seg, uint16_t off) {
  uint16_t val;
  __asm__ volatile(
    "push %%es\n\t"
    "mov  %1, %%es\n\t"
    "mov  %%es:(%%bx), %0\n\t"
    "pop  %%es"
    : "=a"(val)
    : "r"(seg), "b"(off)
    : "memory"
  );
  return val;
}

/* Core entry point offsets (from core_entries.S, order matches mod_core.inc) */
extern uint16_t kmem_pool_init_entry;
extern uint16_t kmem_alloc_entry;
extern uint16_t kmem_free_entry;
extern uint16_t kmem_free_count_entry;
extern uint16_t mem_region_alloc_entry;
extern uint16_t mem_region_free_entry;
extern uint16_t mem_region_total_bytes_entry;
extern uint16_t mem_region_free_bytes_entry;
extern uint16_t mem_region_page_alloc_entry;
extern uint16_t mem_region_page_free_entry;
extern uint16_t mem_region_page_linear_entry;
extern uint16_t mem_region_page_read_entry;
extern uint16_t mem_region_page_write_entry;
extern uint16_t sched_wakeup_entry;
extern uint16_t sched_switch_entry;
extern uint16_t sched_get_ticks_entry;
extern uint16_t subsys_read_proc_entry;
extern uint16_t svc_set_restart_entry;
extern uint16_t blkdev_read_entry;
extern uint16_t blkdev_write_entry;

static void patch_vfs_fptrs(uint16_t vfs_seg) {
  if (far_read16(vfs_seg, 0) != VFS_HDR_MAGIC) {
    mod_vfs.klogf("SEG: VFS header magic mismatch!\n");
    return;
  }

  uint16_t count = far_read16(vfs_seg, 2);

  /* Patch core→VFS far pointers (in core's vfs_fptrs table) */
  for (uint16_t i = 0; i < count; i++) {
    vfs_fptrs[i * 2]     = far_read16(vfs_seg, 4 + i * 2);
    vfs_fptrs[i * 2 + 1] = vfs_seg;
  }

  /* Patch VFS→core far pointers (in VFS's core_fptrs table at DS=0).
   * The core_fptrs address is stored after the entry offsets
   * in the VFS header (at offset 4 + count*2). */
  uint16_t core_fptrs_addr = far_read16(vfs_seg, 4 + count * 2);
  uint16_t core_seg = seg_get(MOD_ID_CORE);
  volatile uint16_t *cfp = (volatile uint16_t *)(uintptr_t)core_fptrs_addr;

  /* Core entry offsets — order MUST match mod_core.inc indices. */
#define PATCH_CORE(idx, sym) \
  cfp[(idx)*2] = (uint16_t)(uintptr_t)&sym##_entry; \
  cfp[(idx)*2+1] = core_seg
  PATCH_CORE( 0, blkdev_read);
  PATCH_CORE( 1, blkdev_write);
  PATCH_CORE( 2, kmem_alloc);
  PATCH_CORE( 3, kmem_free);
  PATCH_CORE( 4, kmem_free_count);
  PATCH_CORE( 5, kmem_pool_init);
  PATCH_CORE( 6, mem_region_alloc);
  PATCH_CORE( 7, mem_region_free);
  PATCH_CORE( 8, mem_region_free_bytes);
  PATCH_CORE( 9, mem_region_page_alloc);
  PATCH_CORE(10, mem_region_page_free);
  PATCH_CORE(11, mem_region_page_linear);
  PATCH_CORE(12, mem_region_page_read);
  PATCH_CORE(13, mem_region_page_write);
  PATCH_CORE(14, mem_region_total_bytes);
  PATCH_CORE(15, sched_get_ticks);
  PATCH_CORE(16, sched_wakeup);
  PATCH_CORE(17, sched_switch);
  PATCH_CORE(18, subsys_read_proc);
  PATCH_CORE(19, svc_set_restart);
#undef PATCH_CORE
}

static void seg_init_modules(void) {
  volatile mod_info_t *info = (volatile mod_info_t *)MOD_INFO_ADDR;

  /* Page pool start: stage2 stores it in mod[2].segment */
  if (info->mod[2].segment) {
    i16_page_pool_base = (uint32_t)info->mod[2].segment << 4;
  }

  /* Module 0: core — already running in far CS, register segment */
  if (info->count >= 1) {
    seg_register(MOD_ID_CORE, info->mod[0].segment);
  }

  /* Module 1: VFS */
  if (info->count >= 2) {
    uint16_t vfs_seg = info->mod[1].segment;
    seg_register(MOD_ID_VFS, vfs_seg);
    patch_vfs_fptrs(vfs_seg);

    mod_vfs.klogf("SEG: VFS module loaded\n");
  }
}

/* ── Target hooks ────────────────────────────────────────────────────────── */

/* INT 0/6 debug trap — prints faulting CS:IP to COM1, then halts. */
extern void int_debug_handler(void);
__asm__(
  ".code16\n"
  "int_debug_handler:\n"
  "  push %bp\n"
  "  mov  %sp, %bp\n"
  "  mov  4(%bp), %ax\n"     /* faulting IP */
  "  mov  6(%bp), %dx\n"     /* faulting CS */
  "  push %ax\n"
  "  mov  %dx, %ax\n"
  "  call 2f\n"
  "  mov  $0x3A, %al\n"
  "  mov  $0x03F8, %dx\n"
  "  out  %al, %dx\n"
  "  pop  %ax\n"
  "  call 2f\n"
  "  mov  $0x0D, %al\n"
  "  mov  $0x03F8, %dx\n"
  "  out  %al, %dx\n"
  "  mov  $0x0A, %al\n"
  "  out  %al, %dx\n"
  "  hlt\n"
  "  jmp  .\n"
  "2:\n"
  "  push %cx\n"
  "  mov  $4, %cx\n"
  "3: rol  $1, %ax\n"
  "  rol  $1, %ax\n"
  "  rol  $1, %ax\n"
  "  rol  $1, %ax\n"
  "  push %ax\n"
  "  and  $0x0F, %al\n"
  "  add  $0x30, %al\n"
  "  cmp  $0x3A, %al\n"
  "  jb   4f\n"
  "  add  $0x07, %al\n"
  "4: mov  $0x03F8, %dx\n"
  "  out  %al, %dx\n"
  "  pop  %ax\n"
  "  loop 3b\n"
  "  pop  %cx\n"
  "  ret\n"
);

static void install_int_debug(void) {
  volatile uint16_t *ivt = (volatile uint16_t *)0;
  uint16_t off = (uint16_t)(uintptr_t)int_debug_handler;
  uint16_t cs = seg_get(MOD_ID_CORE);
  ivt[0] = off; ivt[1] = cs;   /* INT 0 — divide overflow */
  ivt[12] = off; ivt[13] = cs; /* INT 6 — invalid opcode  */
}

void target_early_init(void)
{
  seg_init_modules();
  install_int_debug();
  /* Logger registration happens in klog_init_logger() called from
   * vfs_init() — all VFS-side, no far call issues on i16. */
}

void target_late_init(void)
{
  /* PIT timer deferred — BIOS INT 13h floppy driver needs the
   * original INT 08h timer for motor control. */
}

int target_mount_rootfs(void)
{
  blkdev_init();
  floppy_blk_init();
  blkdev_t *bd = blkdev_find("fd0");
  if (!bd) {
    mod_vfs.klogf("FLOPPY: fd0 not found\n");
    return -1;
  }
  int rc = mod_vfs.mount_ufs("/", MNT_RDONLY, bd);
  return rc;
}

void target_post_mount(void)
{
  /* PIT timer is deferred — BIOS INT 13h floppy driver needs the
   * original INT 08h handler for motor timeout.  Timer will be
   * started after init exec completes (in kmain). */
}

void target_enable_deferred_timer(void)
{
  static uint8_t pit_started;

  if (!pit_started) {
    timer_init();
    pit_started = 1;
    mod_vfs.klogf("PIT: timer started\n");
  }
}

const char *target_init_path(void)
{
#ifdef PPAP_TESTS
#ifdef PPAP_TESTS_EXTENDED
  return "/bin/runtests_ext";
#else
  return "/bin/runtests";
#endif
#else
  return "/sbin/init";
#endif
}

const char *target_name(void)
{
  return "pcxt";
}

uint32_t target_caps(void)
{
  return TARGET_CAP_REALUART;
}
