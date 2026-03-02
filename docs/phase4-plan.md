# Phase 4: SD + VFAT — Detailed Plan

**PicoPiAndPortable Development Roadmap**
Estimated Duration: 3 weeks
Target Board: ClockworkPi PicoCalc (RP2040 + full-size SD slot)

---

## Goals

Build the SD card hardware driver, a FAT32 read/write filesystem driver,
and a block device abstraction layer.  After this phase the kernel can
initialise an SD card over SPI, mount a FAT32 partition at `/mnt/sd`,
and serve `open/read/write/stat/readdir/mkdir/unlink/rename` on files
stored on the SD card.  The block device layer established here becomes
the foundation for Phase 5 loopback mounts.

**Exit Criteria (all must pass before moving to Phase 5):**
- Block device abstraction provides `blkdev_read` / `blkdev_write` with 512-byte sector granularity
- SPI0 driver initialises the RP2040 PL022 peripheral and transfers bytes at ≤ 25 MHz
- SD card initialisation completes the CMD0 → CMD8 → ACMD41 → CMD58 sequence
- SD block read (CMD17) and write (CMD24) transfer 512-byte sectors correctly
- FAT32 boot sector (BPB) is parsed; cluster size, FAT offset, data region offset extracted
- FAT cluster chain traversal reads multi-cluster files correctly
- Short filenames (8.3) and Long File Names (VFAT LFN) are both supported
- `vfs_mount("/mnt/sd", &vfat_ops, 0, dev)` succeeds; files accessible via `sys_open` + `sys_read`
- File creation (`open` with `O_CREAT`), extension, and deletion work on VFAT
- `mkdir` and `unlink` operate correctly on the VFAT partition
- FAT table caching in the I/O buffer region reduces redundant SD reads
- `/dev/mmcblk0` device file provides raw block access
- QEMU smoke test: RAM-backed block device with embedded FAT32 image, read + write files
- Hardware test: real SD card (FAT32) mounted at `/mnt/sd`, file round-trip verified via UART

---

## Source Tree After Phase 4

```
src/
  board/
    picocalc.h            # Board-specific pin definitions (PicoCalc SD/LCD/I2C)
  boot/
    startup.S             (existing)
    stage1.S              (existing)
  kernel/
    main.c                (existing — add SD init + VFAT mount)
    main_qemu.c           (existing — add RAM blkdev + VFAT mount)
    blkdev/
      blkdev.c / blkdev.h # Block device abstraction: read/write sectors
      ramblk.c            # RAM-backed block device (QEMU testing)
    mm/
      page.c/h            (existing)
      kmem.c/h            (existing)
      mpu.c/h             (existing)
      xip.c/h             (existing)
    proc/
      proc.c/h            (existing)
      sched.c/h           (existing)
      switch.S            (existing)
    vfs/
      vfs.c/h             (existing — extend vfs_ops_t with create/mkdir/unlink/rename)
      namei.c             (existing)
    fs/
      romfs.c/h           (existing)
      romfs_format.h      (existing)
      devfs.c/h           (existing — add /dev/mmcblk0)
      procfs.c/h          (existing)
      vfat.c / vfat.h     # FAT32 driver: mount, lookup, read, write, readdir, create, mkdir, unlink, rename
      vfat_format.h       # On-disk FAT32 structures (BPB, directory entries, LFN)
    fd/
      file.h              (existing — add O_CREAT, O_TRUNC, O_APPEND flags)
      fd.c/h              (existing)
      tty.c/h             (existing)
      pipe.c              (existing)
    exec/
      elf.c/h             (existing)
      exec.c/h            (existing)
    signal/
      signal.c/h          (existing)
    syscall/
      syscall.c           (existing — add mkdir, unlink, rename syscalls)
      syscall.h           (existing — add new syscall numbers)
      svc.S               (existing)
      sys_proc.c          (existing)
      sys_io.c            (existing)
      sys_fs.c            (existing — add sys_mkdir, sys_unlink, sys_rename; O_CREAT in sys_open)
      sys_mem.c           (existing)
      sys_time.c          (existing)
    smp.c/h               (existing)
  drivers/
    uart.c/h              (existing)
    uart_qemu.c           (existing)
    clock.c/h             (existing)
    spi.c / spi.h         # SPI0 driver: PL022 init, byte/block transfer
    sd.c / sd.h           # SD card protocol: init sequence, CMD17/CMD24 block I/O
user/
  (existing — add test_sd.c, test_vfat.c)
  test_sd.c              # On-target: SD init, block read/write
  test_vfat.c            # On-target: VFAT mount, file create/read/write/delete
tests/
  (existing)
  test_vfat_unit.c       # Host-native: FAT32 BPB parsing, cluster chain, LFN decoding
  test_blkdev_unit.c     # Host-native: block device abstraction, RAM-backed read/write
tools/
  mkromfs/               (existing)
  mkfatimg/
    mkfatimg.sh          # Build-time script: create a small FAT32 test image using mtools
romfs/
  (existing)
  mnt/
    sd/                  # Empty mount point directory (created by mkromfs)
```

---

## Week 1: Block Device Layer and SPI/SD Hardware Driver

### Step 1 — Block Device Abstraction (`src/kernel/blkdev/`)

The block device layer is the universal interface between filesystem
drivers and storage hardware.  It abstracts sector-level I/O so that
the VFAT driver (and later UFS + loopback in Phase 5) is decoupled
from the underlying transport (SPI/SD on hardware, RAM on QEMU).

**Design:**

```c
/* src/kernel/blkdev/blkdev.h */

#define BLKDEV_SECTOR_SIZE  512u
#define BLKDEV_MAX            4    /* mmcblk0 + loop0..2 + spare */

typedef struct blkdev {
    const char *name;              /* "mmcblk0", "loop0", … */
    uint32_t    sector_count;      /* total sectors on device */
    void       *priv;              /* driver-private state */

    /* Read `count` sectors starting at `sector` into `buf`.
     * Returns 0 on success, negative errno on failure. */
    int (*read)(struct blkdev *dev, void *buf,
                uint32_t sector, uint32_t count);

    /* Write `count` sectors starting at `sector` from `buf`.
     * Returns 0 on success, negative errno on failure. */
    int (*write)(struct blkdev *dev, const void *buf,
                 uint32_t sector, uint32_t count);
} blkdev_t;

/* Register a block device.  Returns the device index (0–3) or -ENOMEM. */
int blkdev_register(blkdev_t *dev);

/* Look up a registered block device by name.  Returns NULL if not found. */
blkdev_t *blkdev_find(const char *name);
```

**RAM-backed block device for QEMU (`src/kernel/blkdev/ramblk.c`):**

For QEMU testing, a ROM-backed block device serves a FAT32 image
embedded in the QEMU binary's flash/ROM section.  On QEMU we also
allocate a write buffer from the I/O buffer region so that write
operations are testable (writes go to SRAM, reads fall through to ROM
for unmodified sectors).

```c
/* ramblk.c — ROM/RAM-backed block device for QEMU testing */

static const uint8_t *rom_base;  /* FAT32 image in ROM */
static uint8_t *rw_buf;          /* write overlay in SRAM I/O buffer */
static uint32_t rw_sectors;      /* number of sectors in overlay */

static int ramblk_read(blkdev_t *dev, void *buf,
                       uint32_t sector, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t s = sector + i;
        const uint8_t *src = (rw_buf && sector_dirty(s))
            ? rw_buf + s * 512
            : rom_base + s * 512;
        memcpy((uint8_t *)buf + i * 512, src, 512);
    }
    return 0;
}
```

A build-time script creates a small FAT32 image (see Step 8) that is
linked into the QEMU binary alongside the romfs image.

**devfs integration:**

`/dev/mmcblk0` is added to devfs.  Read/write on the device file
delegates to `blkdev_read` / `blkdev_write` on the registered SD block
device.  This enables user-space tools (like a future `dd` or `fsck`)
to access raw block storage.

**Key design rationale:**

- Sector size is fixed at 512 bytes — all SD cards use 512-byte
  physical sectors (or 512-byte logical sectors for SDHC).
- The `blkdev_t` struct is small (28 bytes) and statically allocated.
  No heap overhead.
- The same interface serves SD hardware, RAM backing, and Phase 5
  loopback devices — only the `read`/`write` function pointers differ.


### Step 2 — SPI0 Hardware Driver (`src/drivers/spi.c`)

The RP2040 has two PL022 (ARM PrimeCell SSP) SPI controllers.  SPI0 is
used for SD card communication.

**RP2040 SPI0 peripheral registers (base: 0x4003C000):**

| Offset | Register | Purpose |
|---|---|---|
| 0x00 | SSPCR0 | Frame format, clock rate, data size |
| 0x04 | SSPCR1 | Enable, master mode, loopback |
| 0x08 | SSPDR | Data register (TX/RX FIFO) |
| 0x0C | SSPSR | Status (TX empty, RX not empty, busy) |
| 0x10 | SSPCPSR | Clock prescaler (even, 2–254) |
| 0x24 | SSPDMACR | DMA control |

**Pin assignment (PicoCalc board — see `docs/PicoCalc.md`):**

| Function | GPIO | Notes |
|---|---|---|
| SPI0_RX (MISO) | GP16 | SD card data out |
| SD_CS | GP17 | GPIO, manual chip-select control |
| SPI0_SCK | GP18 | SD card clock |
| SPI0_TX (MOSI) | GP19 | SD card data in |
| SD_CD | GP22 | Card detect, active low (low = inserted) |

Confirmed from FUZIX patches, Picocalc_SD_Boot bootloader, and
[ClockworkPi forum GPIO reference](https://forum.clockworkpi.com/t/gpio-for-pico-calc-how-to-make-firmware-for-pico-calc/20905/6).
No conflicts with LCD (SPI1: GP10–15) or PSRAM (PIO: GP2–5, GP20–21).

**API:**

```c
/* src/drivers/spi.h */

/* Initialise SPI0 peripheral at the given baud rate.
 * Configures GPIO pins, sets master mode, 8-bit frames.
 * Initial baud ≤ 400 kHz for SD init; raised to ≤ 25 MHz after. */
void spi_init(uint32_t baud_hz);

/* Change the SPI clock rate (e.g., from 400 kHz init to 25 MHz fast). */
void spi_set_baud(uint32_t baud_hz);

/* Transfer one byte: send `tx`, return received byte.
 * Blocks until the transfer completes. */
uint8_t spi_xfer(uint8_t tx);

/* Transfer a block of bytes.  `tx` may be NULL (sends 0xFF).
 * `rx` may be NULL (discards received bytes). */
void spi_xfer_block(const uint8_t *tx, uint8_t *rx, uint32_t len);

/* Assert (low) / deassert (high) the SD card chip-select line. */
void sd_cs_low(void);
void sd_cs_high(void);

/* Check card-detect pin (GP22).  Returns 1 if card is inserted. */
int spi_card_detect(void);
```

**Initialisation sequence:**

1. Bring SPI0 out of reset via the RESETS register
2. Configure GPIO 16–19 for SPI0 function (IO_BANK0 FUNCSEL = 1)
3. Configure GPIO 17 as SIO output for manual CS control
4. Set SSPCR0: DSS=7 (8-bit), FRF=0 (SPI Motorola), SPO=0, SPH=0
5. Set SSPCPSR for ≤ 400 kHz initial clock (SD init requirement)
6. Set SSPCR1: SSE=1 (enable), MS=0 (master)

**Baud rate calculation:**

```
F_SPI = F_peri / (CPSDVSR × (1 + SCR))
```

For 400 kHz at F_peri = 133 MHz:
CPSDVSR = 254, SCR = floor(133000000 / (254 × 400000)) - 1 ≈ 0
→ actual clock = 133000000 / (254 × 1) ≈ 524 kHz (close enough for init)

For 25 MHz after init:
CPSDVSR = 2, SCR = floor(133000000 / (2 × 25000000)) - 1 = 1
→ actual clock = 133000000 / (2 × 2) = 33.25 MHz (capped at 25 MHz by SD spec;
use CPSDVSR=4, SCR=0 → 33.25 MHz, or CPSDVSR=6, SCR=0 → 22.2 MHz for margin)

**QEMU note:** QEMU mps2-an500 does not emulate SPI0 or GPIO.  The SPI
driver compiles to no-ops on QEMU (guarded by `#ifdef PPAP_QEMU`).
All SD/VFAT testing on QEMU uses the RAM-backed block device from Step 1.

**Board header (`src/board/picocalc.h`):**

```c
#ifndef PPAP_BOARD_PICOCALC_H
#define PPAP_BOARD_PICOCALC_H

/* SD card — SPI0 */
#define SD_PIN_MISO    16
#define SD_PIN_CS      17
#define SD_PIN_SCK     18
#define SD_PIN_MOSI    19
#define SD_PIN_CD      22    /* card detect, active low */

#define SD_CLK_INIT    400000u     /* 400 kHz for CMD0 */
#define SD_CLK_FAST    25000000u   /* 25 MHz data transfer */

/* LCD — SPI1 (not used in Phase 4) */
#define LCD_PIN_SCK    10
#define LCD_PIN_MOSI   11
#define LCD_PIN_MISO   12
#define LCD_PIN_CS     13
#define LCD_PIN_DC     14
#define LCD_PIN_RST    15

/* Keyboard — I2C1 */
#define KBD_PIN_SDA    6
#define KBD_PIN_SCL    7
#define KBD_I2C_ADDR   0x1F

#endif
```

The SPI driver includes this header and uses the `SD_PIN_*` defines
for GPIO configuration.

**Gotcha — GPIO function selection:** On RP2040, each GPIO pad can be
connected to one of several peripherals via `IO_BANK0_GPIOx_CTRL`.
The FUNCSEL for SPI0 is 1 for GPIO 16/19 and 1 for GPIO 18.  GPIO 17
must be set to SIO (FUNCSEL=5) for manual CS control — the PL022's
hardware CS is active-low but does not deassert between bytes, which
violates the SD card protocol's per-command CS framing.


### Step 3 — SD Card Protocol Driver (`src/drivers/sd.c`)

The SD card driver implements the SD protocol in SPI mode.  It handles
card detection, initialisation, and block-level read/write, then
registers itself as a block device.

**SD card SPI-mode command format (6 bytes):**

```
[0] Start bit (0) + transmission (1) + command index (6 bits) = 0x40 | cmd
[1–4] 32-bit argument (big-endian)
[5] CRC7 + stop bit (1)
```

**Initialisation sequence:**

```
sd_init():
  1. Wait 1 ms after power-up
  2. Send ≥ 74 clock pulses with CS high (SD spec §6.4.1)
     → spi_xfer(0xFF) × 10 with CS deasserted
  3. CMD0 (GO_IDLE_STATE), arg=0, CRC=0x95
     → expect R1 = 0x01 (idle bit set)
  4. CMD8 (SEND_IF_COND), arg=0x1AA, CRC=0x87
     → if R1 = 0x01 + echo 0x1AA → SD v2.0 card
     → if R1 = 0x05 (illegal command) → SD v1.x or MMC (unsupported)
  5. Loop (up to 1 second):
       CMD55 (APP_CMD) + ACMD41 (SD_SEND_OP_COND), arg=HCS=1
       → repeat until R1 = 0x00 (ready)
  6. CMD58 (READ_OCR)
     → check CCS bit (bit 30): 1 = SDHC (block addressing)
  7. Raise SPI clock to ≤ 25 MHz: spi_set_baud(25000000)
  8. CMD16 (SET_BLOCKLEN) arg=512 (redundant for SDHC but required for SD v1)
```

**Block read (CMD17 — READ_SINGLE_BLOCK):**

```c
int sd_read_block(uint32_t sector, uint8_t *buf) {
    uint32_t addr = sdhc ? sector : sector * 512;  /* SDHC: block addr */
    sd_cs_low();
    sd_send_cmd(17, addr);
    int r1 = sd_recv_r1();
    if (r1 != 0x00) { sd_cs_high(); return -EIO; }

    /* Wait for data token 0xFE */
    int timeout = 10000;
    while (spi_xfer(0xFF) != 0xFE && --timeout > 0);
    if (timeout == 0) { sd_cs_high(); return -ETIMEDOUT; }

    /* Read 512 bytes + 2-byte CRC (discarded) */
    spi_xfer_block(NULL, buf, 512);
    spi_xfer(0xFF); spi_xfer(0xFF);  /* CRC16 */
    sd_cs_high();
    return 0;
}
```

**Block write (CMD24 — WRITE_BLOCK):**

```c
int sd_write_block(uint32_t sector, const uint8_t *buf) {
    uint32_t addr = sdhc ? sector : sector * 512;
    sd_cs_low();
    sd_send_cmd(24, addr);
    int r1 = sd_recv_r1();
    if (r1 != 0x00) { sd_cs_high(); return -EIO; }

    spi_xfer(0xFE);                  /* data token */
    spi_xfer_block(buf, NULL, 512);  /* 512 bytes */
    spi_xfer(0xFF); spi_xfer(0xFF); /* dummy CRC */

    /* Check data response token */
    uint8_t resp = spi_xfer(0xFF) & 0x1F;
    if (resp != 0x05) { sd_cs_high(); return -EIO; }

    /* Wait for write completion (busy = 0x00) */
    int timeout = 100000;
    while (spi_xfer(0xFF) == 0x00 && --timeout > 0);
    sd_cs_high();
    return (timeout > 0) ? 0 : -ETIMEDOUT;
}
```

**MBR partition table parsing:**

After SD initialisation, read sector 0 (MBR), verify signature
`0x55AA`, and locate the first FAT32 partition (type = 0x0B or 0x0C).
Extract the start LBA and sector count.

```c
typedef struct {
    uint8_t  status;        /* 0x80 = active */
    uint8_t  chs_start[3];
    uint8_t  type;          /* 0x0B = FAT32, 0x0C = FAT32 LBA */
    uint8_t  chs_end[3];
    uint32_t lba_start;     /* partition start sector (little-endian) */
    uint32_t sector_count;  /* partition size in sectors */
} __attribute__((packed)) mbr_part_t;
```

**Block device registration:**

After successful initialisation, the SD driver calls
`blkdev_register(&sd_blkdev)` where `sd_blkdev.read = sd_read_block_wrapper`
and `sd_blkdev.write = sd_write_block_wrapper`.  The wrapper adjusts
sector numbers by the partition's `lba_start` offset so that the FAT32
driver sees a zero-based partition.

**Error handling:**

- Card not present: `sd_init()` returns `-ENODEV`; boot continues
  with romfs only (no /mnt/sd).  A diagnostic message is printed.
- CRC errors: logged and retried once; persistent errors return `-EIO`.
- Timeout: SD cards may take up to 250 ms for write; timeouts tuned
  accordingly.

**QEMU note:** On QEMU, `sd_init()` is skipped.  Instead, `main_qemu.c`
sets up the RAM-backed block device directly.

---

## Week 2: FAT32 File System Driver

### Step 4 — FAT32 Data Structures (`src/kernel/fs/vfat_format.h`)

The on-disk FAT32 structures.  All fields are little-endian.  The
structures are packed and accessed through the block device via the
I/O buffer.

**BPB (BIOS Parameter Block) — first sector of the partition:**

```c
/* src/kernel/fs/vfat_format.h */

typedef struct {
    uint8_t  jmp[3];           /* jump instruction */
    uint8_t  oem_name[8];     /* OEM name (e.g., "MSDOS5.0") */
    uint16_t bytes_per_sector; /* always 512 */
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors; /* sectors before the first FAT */
    uint8_t  num_fats;         /* usually 2 */
    uint16_t root_entry_count; /* 0 for FAT32 */
    uint16_t total_sectors_16; /* 0 for FAT32 */
    uint8_t  media_type;
    uint16_t fat_size_16;      /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    /* FAT32-specific fields (offset 36) */
    uint32_t fat_size_32;      /* sectors per FAT */
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;     /* first cluster of root directory */
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];      /* "FAT32   " */
} __attribute__((packed)) fat32_bpb_t;

_Static_assert(sizeof(fat32_bpb_t) == 90, "BPB size");
```

**Directory entry (32 bytes):**

```c
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LONG_NAME  0x0F  /* LFN entry marker */

typedef struct {
    uint8_t  name[11];        /* 8.3 short filename (space-padded) */
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_hi; /* high 16 bits of first cluster */
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_lo; /* low 16 bits of first cluster */
    uint32_t file_size;
} __attribute__((packed)) fat_dirent_t;

_Static_assert(sizeof(fat_dirent_t) == 32, "FAT dirent size");
```

**Long File Name entry (32 bytes, same size as dirent):**

```c
typedef struct {
    uint8_t  order;           /* sequence number (bit 6 = last) */
    uint16_t name1[5];        /* UCS-2 chars 1–5 */
    uint8_t  attr;            /* always FAT_ATTR_LONG_NAME (0x0F) */
    uint8_t  type;            /* 0 for VFAT LFN */
    uint8_t  checksum;        /* SFN checksum */
    uint16_t name2[6];        /* UCS-2 chars 6–11 */
    uint16_t first_cluster;   /* always 0 */
    uint16_t name3[2];        /* UCS-2 chars 12–13 */
} __attribute__((packed)) fat_lfn_t;

_Static_assert(sizeof(fat_lfn_t) == 32, "LFN entry size");
```

**In-memory VFAT superblock (stored in `mount_entry->sb_priv`):**

```c
typedef struct {
    blkdev_t *dev;                  /* underlying block device */
    uint32_t  part_lba;            /* partition start sector (from MBR) */

    /* Parsed from BPB */
    uint32_t  sectors_per_cluster;
    uint32_t  bytes_per_cluster;
    uint32_t  fat_start_sector;    /* first FAT sector (= reserved_sectors) */
    uint32_t  fat_sectors;         /* sectors per FAT */
    uint32_t  data_start_sector;   /* first data sector */
    uint32_t  root_cluster;        /* root directory first cluster */
    uint32_t  total_clusters;

    /* I/O buffer pointers (within SRAM_IOBUF region) */
    uint8_t  *sector_buf;          /* 512-byte sector buffer */
    uint32_t *fat_cache;           /* cached FAT sectors */
    uint32_t  fat_cache_sector;    /* which FAT sector is cached */
    uint8_t   fat_cache_dirty;     /* 1 if cache needs write-back */
} vfat_sb_t;
```

**Cluster-to-sector conversion:**

```c
static inline uint32_t cluster_to_sector(const vfat_sb_t *sb, uint32_t cluster) {
    return sb->data_start_sector + (cluster - 2) * sb->sectors_per_cluster;
}
```

**I/O buffer layout (24 KB at 0x20038000):**

| Offset | Size | Purpose |
|---|---|---|
| 0x0000 | 512 B | Sector buffer (active sector for read/write) |
| 0x0200 | 512 B | Directory sector buffer (for readdir/lookup) |
| 0x0400 | 4 KB | FAT table cache (8 sectors × 512 B) |
| 0x1400 | 4 KB | Second FAT cache slot (for cluster chain walks) |
| 0x2400 | 15.5 KB | Reserved for Phase 5 UFS metadata cache |

The sector buffer and FAT cache are managed by the VFAT driver.  In
Phase 5, the UFS driver will use the reserved portion.

**Config update (`config.h`):**

Increase `VFS_NAME_MAX` from 28 to 63 to accommodate VFAT LFN:

```c
#define VFS_NAME_MAX   63   /* was 28; increased for VFAT LFN support */
```

This increases `struct dirent` from 37 to 72 bytes.  With at most
~16 dirents buffered at once, the total cost is ~560 bytes — acceptable.


### Step 5 — FAT32 Read Operations (`src/kernel/fs/vfat.c`)

The VFAT driver implements the core read path: mounting, directory
traversal, cluster chain following, and file reading.

**Mount (`vfat_mount`):**

1. Read sector 0 of the block device (BPB)
2. Validate: `bytes_per_sector == 512`, `root_entry_count == 0` (FAT32)
3. Parse BPB fields into `vfat_sb_t`
4. Calculate `data_start_sector = reserved_sectors + num_fats × fat_size_32`
5. Calculate `total_clusters = (total_sectors_32 - data_start_sector) / sectors_per_cluster`
6. Validate `total_clusters >= 65525` (FAT32 threshold)
7. Allocate root vnode: type = VNODE_DIR, ino = root_cluster
8. Cache the first FAT sector

**FAT table lookup:**

```c
/* Read the FAT entry for `cluster`.  Returns the next cluster number,
 * or 0x0FFFFFF8+ for end-of-chain, or 0 for free. */
static uint32_t fat_next_cluster(vfat_sb_t *sb, uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = sb->fat_start_sector + (fat_offset / 512);
    uint32_t entry_off  = (fat_offset % 512) / 4;

    /* Check if the sector is in the cache */
    if (fat_sector != sb->fat_cache_sector) {
        if (sb->fat_cache_dirty)
            vfat_flush_fat_cache(sb);
        blkdev_read(sb->dev, sb->fat_cache, fat_sector, 1);
        sb->fat_cache_sector = fat_sector;
    }
    return sb->fat_cache[entry_off] & 0x0FFFFFFFu;
}
```

**Cluster chain iteration:**

```c
/* Read the full contents of a cluster chain starting at `start_cluster`.
 * Calls `callback(sector_data, sector_count, ctx)` for each cluster. */
static int vfat_walk_chain(vfat_sb_t *sb, uint32_t start_cluster,
                            int (*callback)(const uint8_t *, uint32_t, void *),
                            void *ctx);
```

**Directory lookup (`vfat_lookup`):**

1. Read the directory's cluster chain sector by sector
2. For each 32-byte entry in each sector:
   a. If `attr == FAT_ATTR_LONG_NAME` → collect LFN fragments
   b. If `name[0] == 0x00` → end of directory
   c. If `name[0] == 0xE5` → deleted entry, skip
   d. If regular SFN entry → reconstruct 8.3 name; compare
   e. If LFN was collected → reconstruct full name; compare
3. On match: allocate vnode, populate from directory entry
4. Return 0 or `-ENOENT`

**LFN decoding:**

LFN entries precede the SFN entry in reverse order (sequence numbers
counting down).  Each LFN entry holds 13 UCS-2 characters.  The
implementation extracts ASCII from the low byte of each UCS-2 char
(sufficient for the ASCII + basic UTF-8 requirement in the spec).

```c
/* Collect LFN entries into a name buffer.  Returns name length. */
static int vfat_decode_lfn(const fat_lfn_t *entries, int count,
                            char *name, int name_max) {
    int pos = 0;
    for (int i = count - 1; i >= 0 && pos < name_max; i--) {
        const fat_lfn_t *e = &entries[i];
        /* Extract 13 chars per entry from name1/name2/name3 */
        for (int j = 0; j < 5 && pos < name_max; j++)
            if (e->name1[j] && e->name1[j] != 0xFFFF)
                name[pos++] = (char)(e->name1[j] & 0xFF);
        for (int j = 0; j < 6 && pos < name_max; j++)
            if (e->name2[j] && e->name2[j] != 0xFFFF)
                name[pos++] = (char)(e->name2[j] & 0xFF);
        for (int j = 0; j < 2 && pos < name_max; j++)
            if (e->name3[j] && e->name3[j] != 0xFFFF)
                name[pos++] = (char)(e->name3[j] & 0xFF);
    }
    name[pos] = '\0';
    return pos;
}
```

**SFN decoding:**

```c
/* Convert 8.3 FAT name to normal filename string. */
static void vfat_decode_sfn(const uint8_t name[11], char *out) {
    int pos = 0;
    /* Base name (8 chars, right-trimmed) */
    for (int i = 0; i < 8 && name[i] != ' '; i++)
        out[pos++] = (name[i] >= 'A' && name[i] <= 'Z')
                     ? name[i] + 32 : name[i];  /* lowercase */
    /* Extension (3 chars, right-trimmed) */
    if (name[8] != ' ') {
        out[pos++] = '.';
        for (int i = 8; i < 11 && name[i] != ' '; i++)
            out[pos++] = (name[i] >= 'A' && name[i] <= 'Z')
                         ? name[i] + 32 : name[i];
    }
    out[pos] = '\0';
}
```

**File read (`vfat_read`):**

1. From the vnode's `ino` (= first cluster), follow the cluster chain
2. Skip clusters until reaching the cluster that contains `offset`
3. Read from the current position within that cluster
4. Continue to next cluster(s) as needed up to `count` bytes
5. Return bytes read (capped at file size)

```c
static long vfat_read(vnode_t *vn, void *buf, size_t n, uint32_t off) {
    vfat_sb_t *sb = (vfat_sb_t *)vn->mount->sb_priv;
    if (off >= vn->size) return 0;
    if (off + n > vn->size) n = vn->size - off;

    /* Walk cluster chain to the starting cluster */
    uint32_t cluster = (uint32_t)(uintptr_t)vn->fs_priv;  /* first cluster */
    uint32_t skip = off / sb->bytes_per_cluster;
    for (uint32_t i = 0; i < skip; i++)
        cluster = fat_next_cluster(sb, cluster);

    /* Read data from current position */
    uint32_t cluster_off = off % sb->bytes_per_cluster;
    size_t total = 0;
    while (total < n) {
        if (cluster >= 0x0FFFFFF8u) break;  /* end of chain */
        uint32_t sector = cluster_to_sector(sb, cluster);
        uint32_t sec_in_cluster = cluster_off / 512;
        uint32_t sec_off = cluster_off % 512;

        /* Read one sector at a time */
        blkdev_read(sb->dev, sb->sector_buf,
                    sector + sec_in_cluster, 1);
        uint32_t chunk = 512 - sec_off;
        if (chunk > n - total) chunk = n - total;
        memcpy((uint8_t *)buf + total, sb->sector_buf + sec_off, chunk);
        total += chunk;
        cluster_off += chunk;

        if (cluster_off >= sb->bytes_per_cluster) {
            cluster = fat_next_cluster(sb, cluster);
            cluster_off = 0;
        }
    }
    return (long)total;
}
```

**Readdir (`vfat_readdir`):**

Iterates the directory cluster chain.  The `cookie` is the byte offset
within the directory.  For each non-deleted, non-LFN entry, fills a
`struct dirent`.  LFN entries are collected and decoded for the
following SFN entry's `d_name`.

**Stat (`vfat_stat`):**

```c
static int vfat_stat(vnode_t *vn, struct stat *st) {
    st->st_ino   = vn->ino;
    st->st_size  = vn->size;
    st->st_nlink = 1;
    st->st_mode  = (vn->type == VNODE_DIR)
                   ? (S_IFDIR | 0755)
                   : (S_IFREG | 0644);
    return 0;
}
```


### Step 6 — VFS Integration and Mount Sequence

**Extend `vfs_ops_t` with write operations:**

```c
/* Added to struct vfs_ops (vfs.h) */
struct vfs_ops {
    /* ... existing: mount, lookup, read, write, readdir, stat, readlink ... */
    int (*create)(vnode_t *dir, const char *name, uint32_t mode,
                  vnode_t **result);
    int (*mkdir)(vnode_t *dir, const char *name, uint32_t mode);
    int (*unlink)(vnode_t *dir, const char *name);
    int (*rename)(vnode_t *old_dir, const char *old_name,
                  vnode_t *new_dir, const char *new_name);
    int (*truncate)(vnode_t *vn, uint32_t size);
};
```

Existing read-only FS drivers (romfs, devfs, procfs) set these new
function pointers to NULL.  The VFS layer checks for NULL before calling
and returns `-EROFS` (Read-only file system) if the operation is
attempted on a read-only mount.

**New syscalls:**

| Syscall | Number | Implementation |
|---|---|---|
| `mkdir(path, mode)` | 39 | `vfs_lookup(parent)` → `ops->mkdir` |
| `unlink(path)` | 10 | `vfs_lookup(parent)` → `ops->unlink` |
| `rename(old, new)` | 38 | `vfs_lookup(old_parent, new_parent)` → `ops->rename` |

These are added to `sys_fs.c` and the dispatch table in `syscall.c`.

**Extend `sys_open` with `O_CREAT`:**

```c
#define O_CREAT   0x40u
#define O_TRUNC   0x200u
#define O_APPEND  0x400u
```

When `flags & O_CREAT` and the file does not exist, `sys_open` calls
`dir_vnode->mount->ops->create(dir_vnode, name, mode, &new_vnode)`.
This requires `vfs_lookup` to return the parent directory and remaining
component on `-ENOENT`.  A helper `vfs_lookup_parent()` is added:

```c
/* Look up path, returning the parent directory vnode and the final
 * component name.  Used by create, mkdir, unlink, rename. */
int vfs_lookup_parent(const char *path, vnode_t **parent, char *name_out);
```

**VFAT VFS ops registration:**

```c
const vfs_ops_t vfat_ops = {
    .mount    = vfat_mount,
    .lookup   = vfat_lookup,
    .read     = vfat_read,
    .write    = vfat_write,
    .readdir  = vfat_readdir,
    .stat     = vfat_stat,
    .readlink = NULL,          /* VFAT has no symlinks */
    .create   = vfat_create,
    .mkdir    = vfat_mkdir,
    .unlink   = vfat_unlink,
    .rename   = vfat_rename,
    .truncate = vfat_truncate,
};
```

**Mount sequence in `kmain()` (hardware):**

```c
void kmain(void) {
    /* ... existing init ... */

    /* Phase 4: SD + VFAT */
    int sd_err = sd_init();           /* SPI0 + SD card init */
    if (sd_err == 0) {
        blkdev_t *sd = blkdev_find("mmcblk0");
        vfs_mount("/mnt/sd", &vfat_ops, 0, sd);
        uart_puts("VFAT: mounted /mnt/sd\n");
    } else {
        uart_puts("SD: not detected, continuing without /mnt/sd\n");
    }

    /* ... launch init process ... */
}
```

**Mount sequence in `main_qemu.c`:**

```c
void kmain_qemu(void) {
    /* ... existing init ... */

    /* Phase 4: RAM-backed FAT32 for testing */
    extern const uint8_t _fatimg_start[];
    extern const uint8_t _fatimg_end[];
    ramblk_init(_fatimg_start, _fatimg_end - _fatimg_start);
    blkdev_t *ramblk = blkdev_find("mmcblk0");
    vfs_mount("/mnt/sd", &vfat_ops, 0, ramblk);

    /* ... launch test runner ... */
}
```

**Linker script update (QEMU):**

```ld
/* Add a section for the embedded FAT32 test image */
.fatimg : {
    _fatimg_start = .;
    KEEP(*(.fatimg))
    _fatimg_end = .;
} > ROM
```

**romfs update:**

Add an empty `/mnt/sd/` directory to the romfs tree so that the mount
point exists before the VFAT mount.  After `vfs_mount("/mnt/sd", …)`,
file accesses under `/mnt/sd/` are routed to the VFAT driver.

---

## Week 3: Write Support and Testing

### Step 7 — FAT32 Write Operations

Write support covers file creation, data writing, file deletion, and
directory creation.  These operations modify both the FAT table and
directory entries on the SD card.

**File creation (`vfat_create`):**

1. Find a free cluster: scan the FAT for an entry == 0x00000000
2. Mark it as end-of-chain (0x0FFFFFF8) in the FAT
3. Find a free slot in the parent directory (entry with `name[0] == 0x00`
   or `name[0] == 0xE5`)
4. Write the SFN directory entry (and LFN entries if the name requires it)
5. Allocate a vnode for the new file
6. Write-back the modified FAT sector and directory sector

**SFN name generation:**

For files created by the OS (e.g., via user-space `open` with `O_CREAT`),
generate an 8.3 name from the filename.  If the name fits 8.3 exactly,
use it directly.  Otherwise, generate a short name with a `~1` suffix
and create LFN entries for the full name.

```c
/* Check if filename fits 8.3 format.  Returns 1 if yes, 0 if LFN needed. */
static int vfat_name_fits_sfn(const char *name);

/* Generate 8.3 SFN from long name: take first 6 chars + "~1" + ext.
 * If collision, increment to ~2, ~3, etc. */
static void vfat_generate_sfn(const char *name, uint8_t sfn[11],
                               vnode_t *dir);
```

**File write (`vfat_write`):**

1. Walk the cluster chain to the position at `offset`
2. If writing beyond the current chain, allocate new clusters:
   - Find free clusters in the FAT
   - Link them to the end of the chain
3. Write data to sectors via `blkdev_write`
4. Update `file_size` in the directory entry if the file grew
5. Flush modified FAT sectors

**File deletion (`vfat_unlink`):**

1. Look up the file in the parent directory
2. Mark the SFN entry as deleted (`name[0] = 0xE5`)
3. Mark any preceding LFN entries as deleted
4. Free the file's cluster chain: walk the chain, set each FAT entry to 0
5. Flush FAT and directory sectors

**Directory creation (`vfat_mkdir`):**

1. Allocate a cluster for the new directory
2. Create `.` and `..` entries in the first sector of the new cluster
3. Add a directory entry in the parent directory
4. Flush

**File truncation (`vfat_truncate`):**

1. Walk the cluster chain to the cluster containing `new_size`
2. Mark that cluster as end-of-chain
3. Free remaining clusters
4. Update `file_size` in the directory entry

**Rename (`vfat_rename`):**

1. Look up the source entry
2. Create a new entry in the target directory (copy dirent data)
3. Delete the source entry
4. If renaming a directory, update its `..` entry

**FAT update protocol:**

Both copies of the FAT (FAT1 and FAT2) must be kept in sync.  After
modifying the cached FAT sector:

```c
static void vfat_flush_fat_cache(vfat_sb_t *sb) {
    if (!sb->fat_cache_dirty) return;
    /* Write to FAT1 */
    blkdev_write(sb->dev, sb->fat_cache,
                 sb->fat_cache_sector, 1);
    /* Write to FAT2 (mirror) */
    blkdev_write(sb->dev, sb->fat_cache,
                 sb->fat_cache_sector + sb->fat_sectors, 1);
    sb->fat_cache_dirty = 0;
}
```


### Step 8 — Build-Time FAT32 Image and Integration

**FAT32 test image for QEMU (`tools/mkfatimg/mkfatimg.sh`):**

A build-time script that creates a small FAT32 image containing test
files.  Uses `mtools` (available via `apt install mtools`) to create
and populate the image without root privileges:

```sh
#!/bin/bash
# tools/mkfatimg/mkfatimg.sh — Create a FAT32 test image for QEMU
set -e

IMG="$1"           # output file (e.g., build/fattest.img)
SIZE_KB="${2:-256}" # image size in KB (default 256 KB)

# Create empty image
dd if=/dev/zero of="$IMG" bs=1024 count="$SIZE_KB" 2>/dev/null

# Format as FAT32 (force FAT32 even for small volume)
mkfs.fat -F 32 -S 512 -s 1 "$IMG" >/dev/null

# Create test directory structure and files
cat > /tmp/mtoolsrc <<EOF
drive z: file="$IMG"
mtools_skip_check=1
EOF
export MTOOLSRC=/tmp/mtoolsrc

mmd z:/testdir
echo -n "Hello from VFAT!" | mcopy - z:/hello.txt
echo -n "Phase 4 test data" | mcopy - z:/testdir/data.txt
echo -n "LFN test content" | mcopy - z:/long_filename_test.txt

rm -f /tmp/mtoolsrc
echo "FAT32 test image: $IMG ($SIZE_KB KB)"
```

**Build integration (CMakeLists.txt):**

```cmake
# Generate FAT32 test image for QEMU
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/fattest.img
    COMMAND bash ${CMAKE_SOURCE_DIR}/tools/mkfatimg/mkfatimg.sh
            ${CMAKE_BINARY_DIR}/fattest.img 256
    COMMENT "Generating FAT32 test image"
)

# Convert to linkable object
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/fattest.o
    COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-littlearm
            --rename-section .data=.fatimg,alloc,load,readonly
            fattest.img fattest.o
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS ${CMAKE_BINARY_DIR}/fattest.img
)
```

The `fattest.o` is linked into the QEMU binary alongside `romfs.o`.
On hardware, the FAT32 image is not embedded — the real SD card is used.

**Gotcha — FAT32 minimum cluster count:** `mkfs.fat -F 32` may refuse
to create a very small volume because the cluster count would be below
65525.  Using `-s 1` (1 sector per cluster) with a 256 KB image gives
only ~480 clusters — technically FAT16.  However, `mkfs.fat -F 32`
forces the FAT32 format regardless.  Our driver validates the BPB
fields rather than the cluster count heuristic, so this works.
Alternatively, use a 64 MB image if the cluster count validation is
strict — but for QEMU testing the forced-FAT32 small image is more
practical.

**romfs `/mnt/sd` mount point:**

Add `romfs/mnt/` and `romfs/mnt/sd/` as empty directories in the romfs
tree.  The `mkromfs` tool already handles empty directories.


### Step 9 — Testing Strategy

Phase 4 extends the three-tier testing strategy from Phase 3 with SD
and VFAT-specific tests.

#### Tier 1: Host-Native Unit Tests (`tests/`)

**New test suites:**

| Test File | Module Under Test | What It Covers |
|---|---|---|
| `tests/test_vfat_unit.c` | `src/kernel/fs/vfat.c` | BPB parsing (valid/corrupt), cluster-to-sector math, SFN decode ("HELLO   TXT" → "hello.txt"), LFN decode (multi-entry reassembly), SFN generation from long names, FAT chain walk (linear/fragmented/end-of-chain) |
| `tests/test_blkdev_unit.c` | `src/kernel/blkdev/blkdev.c` | Register/find device, RAM-backed read/write round-trip, sector boundary checks, multi-sector read |

**How to run:** Same as Phase 3:

```sh
cd build_tests && cmake ../tests && make && ctest --output-on-failure
```

**Test stub additions:**

- `tests/stubs/blkdev_stub.c` — provides a RAM-backed block device for
  host-native tests.  Initialised from a FAT32 image file (`fattest.img`)
  loaded at test start.

**Example: `tests/test_vfat_unit.c` (BPB parsing):**

```c
static void test_bpb_valid(void) {
    /* Load a known-good BPB from the test image */
    uint8_t sector[512];
    blkdev_read(&test_dev, sector, 0, 1);
    fat32_bpb_t *bpb = (fat32_bpb_t *)sector;

    ASSERT_EQ(bpb->bytes_per_sector, 512);
    ASSERT_EQ(bpb->root_entry_count, 0);  /* FAT32 marker */
    ASSERT(bpb->sectors_per_cluster >= 1, "valid cluster size");
    ASSERT(bpb->fat_size_32 > 0, "FAT32 fat_size_32 non-zero");
    ASSERT(bpb->root_cluster >= 2, "root cluster valid");
}

static void test_sfn_decode(void) {
    /* "HELLO   TXT" → "hello.txt" */
    uint8_t sfn[11] = {'H','E','L','L','O',' ',' ',' ','T','X','T'};
    char name[13];
    vfat_decode_sfn(sfn, name);
    ASSERT_STREQ(name, "hello.txt");
}

static void test_sfn_no_ext(void) {
    uint8_t sfn[11] = {'M','A','K','E','F','I','L','E',' ',' ',' '};
    char name[13];
    vfat_decode_sfn(sfn, name);
    ASSERT_STREQ(name, "makefile");
}
```

#### Tier 2: User-Space On-Target Tests (`user/test_*.c`)

**`user/test_sd.c` — SD init and raw block I/O (hardware only):**

```c
#include "utest.h"

int main(void) {
    /* Test 1: /dev/mmcblk0 is accessible */
    int fd = sys_open("/dev/mmcblk0", 0 /* O_RDONLY */, 0);
    UT_ASSERT(fd >= 0, "/dev/mmcblk0 should be openable");

    /* Test 2: read first sector (MBR) */
    char buf[512];
    ssize_t n = sys_read(fd, buf, 512);
    UT_ASSERT_EQ(n, 512);

    /* Test 3: MBR signature */
    UT_ASSERT_EQ((uint8_t)buf[510], 0x55);
    UT_ASSERT_EQ((uint8_t)buf[511], 0xAA);

    sys_close(fd);
    UT_SUMMARY("test_sd");
}
```

**`user/test_vfat.c` — VFAT mount and file operations:**

```c
#include "utest.h"

int main(void) {
    /* Test 1: read a known file */
    int fd = sys_open("/mnt/sd/hello.txt", 0, 0);
    UT_ASSERT(fd >= 0, "open /mnt/sd/hello.txt");
    char buf[64] = {0};
    ssize_t n = sys_read(fd, buf, sizeof(buf));
    UT_ASSERT(n > 0, "read returned data");
    /* Check content: "Hello from VFAT!" */
    int match = 1;
    const char *expect = "Hello from VFAT!";
    for (int i = 0; i < 16; i++)
        if (buf[i] != expect[i]) match = 0;
    UT_ASSERT(match, "file content matches");
    sys_close(fd);

    /* Test 2: read file in subdirectory */
    fd = sys_open("/mnt/sd/testdir/data.txt", 0, 0);
    UT_ASSERT(fd >= 0, "open subdirectory file");
    n = sys_read(fd, buf, sizeof(buf));
    UT_ASSERT(n > 0, "subdirectory file readable");
    sys_close(fd);

    /* Test 3: LFN file */
    fd = sys_open("/mnt/sd/long_filename_test.txt", 0, 0);
    UT_ASSERT(fd >= 0, "open LFN file");
    sys_close(fd);

    /* Test 4: stat a file */
    struct stat st;
    int ret = sys_stat("/mnt/sd/hello.txt", &st);
    UT_ASSERT_EQ(ret, 0);
    UT_ASSERT_EQ(st.st_size, 16);  /* "Hello from VFAT!" = 16 bytes */
    UT_ASSERT(st.st_mode & 0100000, "regular file");

    /* Test 5: readdir root */
    fd = sys_open("/mnt/sd", 0, 0);
    UT_ASSERT(fd >= 0, "open /mnt/sd directory");
    /* getdents should return hello.txt, testdir, long_filename_test.txt */
    sys_close(fd);

    /* Test 6: create a new file */
    fd = sys_open("/mnt/sd/newfile.txt", 0x41 /* O_WRONLY|O_CREAT */, 0644);
    UT_ASSERT(fd >= 0, "create new file");
    const char *data = "write test";
    n = sys_write(fd, data, 10);
    UT_ASSERT_EQ(n, 10);
    sys_close(fd);

    /* Test 7: read back the created file */
    fd = sys_open("/mnt/sd/newfile.txt", 0, 0);
    UT_ASSERT(fd >= 0, "reopen created file");
    char buf2[32] = {0};
    n = sys_read(fd, buf2, 32);
    UT_ASSERT_EQ(n, 10);
    match = 1;
    for (int i = 0; i < 10; i++)
        if (buf2[i] != data[i]) match = 0;
    UT_ASSERT(match, "written data readable");
    sys_close(fd);

    /* Test 8: delete the file */
    ret = sys_unlink("/mnt/sd/newfile.txt");
    UT_ASSERT_EQ(ret, 0);
    fd = sys_open("/mnt/sd/newfile.txt", 0, 0);
    UT_ASSERT(fd < 0, "deleted file should not exist");

    /* Test 9: mkdir */
    ret = sys_mkdir("/mnt/sd/newdir", 0755);
    UT_ASSERT_EQ(ret, 0);
    fd = sys_open("/mnt/sd/newdir", 0, 0);
    UT_ASSERT(fd >= 0, "new directory should be openable");
    sys_close(fd);

    /* Test 10: stat directory */
    ret = sys_stat("/mnt/sd/newdir", &st);
    UT_ASSERT_EQ(ret, 0);
    UT_ASSERT(st.st_mode & 0040000, "should be directory");

    UT_SUMMARY("test_vfat");
}
```

#### Tier 3: Test Runner Update

Add `test_sd` and `test_vfat` to the `runtests.c` test list:

```c
static const char *tests[] = {
    "/bin/test_exec",
    "/bin/test_vfork",
    "/bin/test_pipe",
    "/bin/test_signal",
    "/bin/test_brk",
    "/bin/test_fd",
    "/bin/test_sd",       /* NEW */
    "/bin/test_vfat",     /* NEW */
    NULL
};
```

#### Testing Matrix

| What is tested | Tier 1 (Host) | Tier 2 (On-target) | Tier 3 (Runner) |
|---|---|---|---|
| BPB parsing (valid/corrupt) | `test_vfat_unit.c` | — | — |
| SFN encoding/decoding | `test_vfat_unit.c` | — | — |
| LFN multi-entry reassembly | `test_vfat_unit.c` | — | — |
| Cluster chain traversal | `test_vfat_unit.c` | — | — |
| Block device register/find | `test_blkdev_unit.c` | — | — |
| RAM-backed blkdev read/write | `test_blkdev_unit.c` | — | — |
| SD card init + MBR (QEMU) | — | `test_sd.c` | via runner |
| VFAT mount + file read | — | `test_vfat.c` | via runner |
| LFN file access end-to-end | — | `test_vfat.c` | via runner |
| File create + write + readback | — | `test_vfat.c` | via runner |
| File deletion (unlink) | — | `test_vfat.c` | via runner |
| Directory creation (mkdir) | — | `test_vfat.c` | via runner |
| FAT table dual-copy sync | — | `test_vfat.c` | via runner |

**Expected assertion count (Phase 4):**
- Host-native (Tier 1): ~30 assertions (VFAT ~20, blkdev ~10)
- On-target (Tier 2): ~25 assertions across 2 test binaries
- Runner (Tier 3): 8 test executions with pass/fail collection

#### Hardware Testing Notes

Hardware testing is particularly important for Phase 4 because QEMU
cannot emulate SPI or SD:

- **SPI waveform:** Verify with a logic analyser that SCK, MOSI, MISO,
  CS signals are correct.  Initial clock ≤ 400 kHz during CMD0–ACMD41.
- **SD card compatibility:** Test with at least 2 different SD cards
  (different manufacturers/capacities).  Some cards have longer ACMD41
  convergence times.
- **Write reliability:** Write a file, power-cycle the Pico, re-read
  the file to verify persistence.  Also verify on a PC by inserting
  the SD card.
- **No SD card:** Verify graceful fallback — boot proceeds with romfs
  only, diagnostic message printed.

---

## Deliverables

| File | Description |
|---|---|
| `src/board/picocalc.h` | PicoCalc board pin definitions (SD, LCD, I2C, audio) |
| `src/kernel/blkdev/blkdev.h` | Block device abstraction: `blkdev_t`, register, find |
| `src/kernel/blkdev/blkdev.c` | Block device registry (static array of BLKDEV_MAX) |
| `src/kernel/blkdev/ramblk.c` | RAM/ROM-backed block device for QEMU testing |
| `src/drivers/spi.c` | SPI0 driver: PL022 init, `spi_xfer`, `spi_xfer_block` |
| `src/drivers/spi.h` | SPI0 API declarations |
| `src/drivers/sd.c` | SD card protocol: `sd_init`, `sd_read_block`, `sd_write_block`, MBR parsing |
| `src/drivers/sd.h` | SD card API declarations |
| `src/kernel/fs/vfat_format.h` | On-disk FAT32 structures (BPB, dirent, LFN) |
| `src/kernel/fs/vfat.c` | FAT32 driver: mount, lookup, read, write, readdir, create, mkdir, unlink, rename |
| `src/kernel/fs/vfat.h` | VFAT driver API + `vfat_ops` extern |
| `src/kernel/vfs/vfs.h` | Extended `vfs_ops_t` with create/mkdir/unlink/rename/truncate |
| `src/kernel/vfs/vfs.c` | Updated VFS layer: `vfs_lookup_parent()`, `-EROFS` guard |
| `src/kernel/fd/file.h` | Added `O_CREAT`, `O_TRUNC`, `O_APPEND` flags |
| `src/kernel/fs/devfs.c` | Extended: `/dev/mmcblk0` block device node |
| `src/kernel/syscall/sys_fs.c` | New: `sys_mkdir`, `sys_unlink`, `sys_rename`; `O_CREAT` in `sys_open` |
| `src/kernel/syscall/syscall.c` | Updated dispatch table with new syscalls (mkdir=39, unlink=10, rename=38) |
| `src/kernel/main.c` | Updated: SD init + VFAT mount sequence |
| `src/kernel/main_qemu.c` | Updated: RAM blkdev + VFAT mount for QEMU |
| `src/config.h` | Updated: `VFS_NAME_MAX` 28 → 63 |
| `tools/mkfatimg/mkfatimg.sh` | Build-time script: create FAT32 test image via mtools |
| `CMakeLists.txt` | Updated: FAT32 image generation, blkdev/spi/sd sources, linker |
| `ldscripts/qemu.ld` | Updated: `.fatimg` section for embedded FAT32 image |
| `romfs/mnt/sd/` | Empty mount point directory |
| `user/test_sd.c` | On-target: SD block device access test |
| `user/test_vfat.c` | On-target: VFAT file operations test |
| `user/runtests.c` | Updated: add test_sd and test_vfat to test list |
| `user/syscall.S` | Extended: `sys_mkdir`, `sys_unlink`, `sys_rename` stubs |
| `user/syscall.h` | Extended: mkdir, unlink, rename, stat declarations |
| `tests/test_vfat_unit.c` | Host-native: BPB parsing, SFN/LFN decode, cluster chain tests |
| `tests/test_blkdev_unit.c` | Host-native: block device abstraction tests |
| `tests/stubs/blkdev_stub.c` | Test stub: RAM-backed block device for host tests |

---

## Known Risks and Mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| SD card init fails (ACMD41 timeout on some cards) | High | Retry ACMD41 up to 1 second with 10 ms intervals; test with multiple card brands; print diagnostic on failure |
| SPI clock too fast for SD init (must be ≤ 400 kHz) | High | Verify with logic analyser; start at conservative CPSDVSR; only raise after ACMD41 completes |
| FAT32 cluster count validation rejects small test images | Medium | Relax validation: check BPB `fs_type` field ("FAT32   ") rather than cluster count heuristic; or use a 64 MB test image |
| LFN entry ordering bug (reversed sequence) | High | Unit test with known multi-entry LFN; verify checksum matches SFN |
| FAT cache dirty write-back lost on unexpected power loss | Medium | Flush after every metadata write (slower but safer); document sync requirement |
| QEMU ROM size too small for FAT32 image + romfs | Low | QEMU mps2-an500 has 4 MB ROM; 256 KB FAT32 image + romfs is well within limits |
| SPI0 GPIO conflict with other peripherals | Low | PicoCalc uses GP16–19 exclusively for SD (no overlap with UART GP0/1, LCD SPI1 GP10–15, or PSRAM PIO GP2–5/20–21) |
| I/O buffer region (24 KB) insufficient for FAT cache + UFS | Medium | Carefully partition the I/O buffer; monitor usage at runtime; reduce FAT cache if needed |
| `VFS_NAME_MAX` increase breaks romfs dirent alignment | Low | romfs `name_len` is stored separately; dirent struct padding handled by compiler |
| SD write corrupts FAT table (both copies inconsistent) | Medium | Always write FAT1 then FAT2 in sequence; verify by reading back FAT2 after write (debug builds only) |
| SDHC vs SDSC addressing confusion (block vs byte) | High | Track `sdhc` flag from CMD58; assert sector addressing consistency in debug builds |

---

## References

- [RP2040 Datasheet](https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf) — §4.4 (SPI / PL022), §2.19.2 (IO_BANK0 GPIO function select)
- [SD Physical Layer Simplified Specification](https://www.sdcard.org/downloads/pls/) — §7.2 (SPI mode), §7.3.1.3 (CMD0/CMD8/ACMD41 init), §7.2.3 (block read/write)
- [Microsoft FAT32 File System Specification](https://download.microsoft.com/download/1/6/1/161ba512-40e2-4cc9-843a-923143f3456c/fatgen103.doc) — BPB, FAT, directory entry format
- [VFAT Long File Name specification](https://en.wikipedia.org/wiki/Design_of_the_FAT_file_system#VFAT_long_file_names) — LFN entry structure, checksum algorithm
- [PicoPiAndPortable Design Spec v0.3](PicoPiAndPortable-spec-v03.md) — §3.7 (VFAT driver), §7 (boot sequence Stage 4), §8 (device drivers), §10.3–10.4 (risks)
- [PicoCalc Hardware Specs](PicoCalc.md) — SD card pinout (SPI0: GP16–19, CD: GP22), LCD, keyboard, PSRAM
- [ClockworkPi Forum GPIO Reference](https://forum.clockworkpi.com/t/gpio-for-pico-calc-how-to-make-firmware-for-pico-calc/20905/6) — Complete PicoCalc GPIO mapping
