/*
 * boot_params.h -- Stage2→kernel boot handoff (wire format + globals)
 *
 * stage2 writes a mod_info_t at MOD_INFO_ADDR and jumps into the
 * kernel.  target_pcxt_early_init() reads it and copies the boot
 * device fields into the i16_* globals so drivers (bios_blk.c) and
 * late-init code can reference the boot geometry without re-parsing
 * the handoff structure.
 *
 * Lives under kernel/common/ because both the core module
 * (target_pcxt.c, stage2.c) and the VFS module (bios_blk.c) read
 * these globals; putting the header in kernel/core/ would be a
 * layering violation for the VFS driver.
 */

#ifndef PPAP_TARGET_PCXT_KERNEL_COMMON_BOOT_PARAMS_H
#define PPAP_TARGET_PCXT_KERNEL_COMMON_BOOT_PARAMS_H

#include <stdint.h>

/* ── mod_info handoff ────────────────────────────────────────────────── */

#define MOD_INFO_ADDR 0x0500u /* Free gap 0x0500-0x05FF */
#define MOD_MAX 4u

typedef struct {
  uint16_t count; /* number of loaded modules */
  struct {
    uint16_t segment; /* paragraph address (linear >> 4) */
    uint16_t size;    /* size in bytes */
  } mod[MOD_MAX];
  /* Boot device info */
  uint8_t boot_dev;      /* 0x00..0x7F = floppy, 0x80..0xFF = HDD */
  uint8_t reserved;
  uint16_t ufs_base;     /* absolute LBA of first UFS sector */
  uint16_t dev_spt;      /* sectors per track */
  uint16_t dev_heads;    /* number of heads */
  uint32_t dev_sectors;  /* total partition sectors */
} mod_info_t;

/* ── Boot device globals (populated from mod_info_t at early_init) ─── */

extern uint8_t i16_boot_drive;
extern uint16_t i16_ufs_base_sector;
extern uint16_t i16_dev_spt;
extern uint16_t i16_dev_heads;
extern uint32_t i16_dev_sectors;

#endif /* PPAP_TARGET_PCXT_KERNEL_COMMON_BOOT_PARAMS_H */
