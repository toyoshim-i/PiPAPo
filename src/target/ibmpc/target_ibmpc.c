/*
 * target_ibmpc.c — IBM PC target hooks for the PPAP kernel
 *
 * Phase P-4b: isolated segment split.  Core + VFS modules in
 * separate segments.  Reads mod_info from stage2, initializes
 * segment manager, patches far pointer tables.
 */

#include "drivers/uart.h"
#include "drivers/bios_con.h"
#include "drivers/timer_pit.h"
#include "target/target.h"
#include "klog.h"
#include "common/seg.h"

#ifdef __ia16__

#include "blkdev/blkdev.h"
#include "fs/ufs.h"
#include "common/mod/mod_vfs.h"

extern void floppy_blk_init(void);

/* Far pointer tables from the stub assembly files */
extern uint16_t vfs_fptrs[];  /* in core: caller stubs for VFS */

/* VFS entry point offsets — obtained from the VFS binary's symbol table.
 * These are fixed at build time since the VFS linker script starts at 0. */
extern uint16_t vfs_init_entry;
extern uint16_t vfs_mount_entry;
extern uint16_t vfs_umount_entry;
extern uint16_t vfs_lookup_entry;
extern uint16_t vfs_lookup_flags_entry;
extern uint16_t vfs_lookup_parent_entry;
extern uint16_t vfs_path_normalize_entry;
extern uint16_t vfs_find_mount_entry;
extern uint16_t vfs_alloc_vnode_entry;
extern uint16_t vfs_ref_vnode_entry;
extern uint16_t vfs_rel_vnode_entry;

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

/*
 * Patch the vfs_fptrs table by reading the VFS module's header.
 * The header is at the start of the VFS binary (linear = vfs_seg << 4).
 * Each fptrs entry is [offset:segment] (4 bytes, little-endian).
 */
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

static void patch_vfs_fptrs(uint16_t vfs_seg) {
  /* VFS header is at vfs_seg:0000 */
  if (far_read16(vfs_seg, 0) != VFS_HDR_MAGIC) {
    klog("SEG: VFS header magic mismatch!\n");
    return;
  }

  uint16_t count = far_read16(vfs_seg, 2);
  if (count > 11) count = 11;

  for (uint16_t i = 0; i < count; i++) {
    vfs_fptrs[i * 2]     = far_read16(vfs_seg, 4 + i * 2);
    vfs_fptrs[i * 2 + 1] = vfs_seg;
  }
}

static void seg_init_modules(void) {
  volatile mod_info_t *info = (volatile mod_info_t *)MOD_INFO_ADDR;

  /* Module 0: core — already running, register our own segment */
  if (info->count >= 1) {
    seg_register(MOD_ID_CORE, info->mod[0].segment);
  }

  /* Module 1: VFS */
  if (info->count >= 2) {
    uint16_t vfs_seg = info->mod[1].segment;
    seg_register(MOD_ID_VFS, vfs_seg);
    patch_vfs_fptrs(vfs_seg);
    klog("SEG: VFS module loaded\n");
  }
}

#endif /* __ia16__ */

/* ── klog sink: serial + BIOS screen ─────────────────────────────────────── */

static int ibmpc_klog_putc(char c, void (*notify)(void))
{
  (void)notify;
  bios_putc(c);
  return 1;
}

/* ── Target hooks ────────────────────────────────────────────────────────── */

void target_early_init(void)
{
  uart_init();
  klog_set_mirror(ibmpc_klog_putc, (void (*)(void))0);
  klog("Po booting... [ibmpc]\n");

#ifdef __ia16__
  seg_init_modules();
#endif
}

void target_late_init(void)
{
  timer_init();
  klog("PIT: 100 Hz timer started\n");
}

#ifdef __ia16__
int target_mount_rootfs(void)
{
  floppy_blk_init();
  blkdev_t *bd = blkdev_find("fd0");
  if (!bd) {
    klog("FLOPPY: fd0 not found\n");
    return -1;
  }
  return mod_vfs.mount("/", &ufs_ops, MNT_RDONLY, bd);
}
#endif

void target_post_mount(void)
{
  /* Nothing needed after rootfs mount */
}

const char *target_init_path(void)
{
#ifdef __ia16__
  return "/sbin/init";
#else
  return (const char *)0;
#endif
}

const char *target_name(void)
{
  return "ibmpc";
}

uint32_t target_caps(void)
{
  return TARGET_CAP_REALUART;
}
