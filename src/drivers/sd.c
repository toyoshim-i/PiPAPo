/*
 * sd.c — SD card driver (SPI mode) for RP2040
 *
 * Implements the SD SPI-mode protocol for card initialisation, single-block
 * read (CMD17), single-block write (CMD24), and MBR partition table parsing.
 *
 * After initialisation the driver registers "mmcblk0" via blkdev_register().
 * All I/O uses 512-byte sectors.  SDHC/SDXC cards use block addressing;
 * older SDSC cards use byte addressing (handled transparently).
 *
 * References:
 *   - SD Physical Layer Simplified Spec v9.00, §7 (SPI mode)
 *   - RP2040 Datasheet §4.4 (PL022 SPI)
 */

#include "sd.h"
#include "spi.h"
#include "../kernel/blkdev/blkdev.h"
#include "../kernel/errno.h"
#include "config.h"
#include <stdint.h>
#include <stddef.h>

/* ── SD command indices ──────────────────────────────────────────────────── */

#define CMD0    0    /* GO_IDLE_STATE       */
#define CMD8    8    /* SEND_IF_COND        */
#define CMD16   16   /* SET_BLOCKLEN        */
#define CMD17   17   /* READ_SINGLE_BLOCK   */
#define CMD24   24   /* WRITE_BLOCK         */
#define CMD55   55   /* APP_CMD prefix      */
#define CMD58   58   /* READ_OCR            */
#define ACMD41  41   /* SD_SEND_OP_COND     */

/* R1 response bits */
#define R1_IDLE     (1u << 0)
#define R1_ILLEGAL  (1u << 2)

/* ── SD SPI protocol constants ─────────────────────────────────────────── */

#define SD_CMD_PREFIX       0x40u          /* command byte = 0x40 | cmd_idx  */
#define SD_R1_START_BIT     0x80u          /* R1 valid when bit 7 is 0       */
#define SD_TOKEN_DATA       0xFEu          /* data block start token         */
#define SD_DRESP_MASK       0x1Fu          /* data response mask bits        */
#define SD_DRESP_ACCEPTED   0x05u          /* data response: accepted (010)  */
#define SD_CRC_CMD0         0x95u          /* precomputed CRC7 for CMD0(0)   */
#define SD_CRC_CMD8         0x87u          /* precomputed CRC7 for CMD8(0x1AA) */
#define SD_CRC_DUMMY        0x01u          /* dummy CRC + stop bit           */
#define CMD8_CHECK_PATTERN  0x000001AAu    /* voltage + check pattern        */
#define ACMD41_HCS          0x40000000u    /* HCS bit: host supports SDHC    */
#define OCR_CCS             0x40u          /* CCS bit in OCR byte 0          */
#define SD_SPI_FAST_HZ      25000000u      /* normal-mode SPI clock (25 MHz) */

/* Timeout loop counts */
#define SD_DATA_TIMEOUT     4096           /* data token wait iterations     */
#define SD_WRITE_TIMEOUT    65535          /* write-busy wait iterations     */

/* ── MBR constants ─────────────────────────────────────────────────────── */

#define MBR_SIG_BYTE0       0x55u          /* MBR signature at offset 510    */
#define MBR_SIG_BYTE1       0xAAu          /* MBR signature at offset 511    */
#define MBR_PART_OFFSET     0x1BEu         /* first partition entry offset   */
#define MBR_PART_ENTRY_SIZE 16u            /* bytes per partition entry       */
#define PART_TYPE_FAT32     0x0Bu          /* FAT32 with CHS addressing      */
#define PART_TYPE_FAT32_LBA 0x0Cu          /* FAT32 with LBA addressing      */

/* Delay loop calibration: ~1 ms at 133 MHz (imprecise, sufficient for init) */
#define DELAY_LOOPS_PER_MS  13300u

/* ── Driver state ───────────────────────────────────────────────────────── */

static int sd_sdhc;              /* 1 = SDHC/SDXC (block addressing) */
static uint32_t sd_part_lba;     /* LBA start of first FAT32 partition */
static uint32_t sd_part_sectors; /* sector count of partition */

/* ── Timing helpers ──────────────────────────────────────────────────────── */

/* Busy-wait ~1 ms at 133 MHz (imprecise but sufficient for SD init) */
static void delay_ms(uint32_t ms)
{
    volatile uint32_t count = ms * DELAY_LOOPS_PER_MS;
    while (count--) ;
}

/* ── SD command transport ────────────────────────────────────────────────── */

/*
 * Send a 6-byte SD command and wait for the R1 response byte.
 * Returns the R1 byte, or 0xFF on timeout.
 */
static uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg)
{
    uint8_t frame[6];
    frame[0] = SD_CMD_PREFIX | cmd;
    frame[1] = (uint8_t)(arg >> 24);
    frame[2] = (uint8_t)(arg >> 16);
    frame[3] = (uint8_t)(arg >> 8);
    frame[4] = (uint8_t)(arg);

    /* CRC7 is only checked for CMD0 and CMD8 in SPI mode */
    if (cmd == CMD0)
        frame[5] = SD_CRC_CMD0;
    else if (cmd == CMD8)
        frame[5] = SD_CRC_CMD8;
    else
        frame[5] = SD_CRC_DUMMY;

    spi_xfer_block(frame, NULL, 6);

    /* Wait for response: card drives MISO low (bit 7 = 0) */
    for (int i = 0; i < 10; i++) {
        uint8_t r = spi_xfer(0xFFu);
        if (!(r & SD_R1_START_BIT))
            return r;
    }
    return 0xFFu;   /* timeout */
}

/* Send ACMD (APP_CMD prefix + command) */
static uint8_t sd_send_acmd(uint8_t acmd, uint32_t arg)
{
    sd_send_cmd(CMD55, 0);
    return sd_send_cmd(acmd, arg);
}

/* ── Block I/O ──────────────────────────────────────────────────────────── */

/*
 * Read a single 512-byte sector from the SD card.
 * `addr` is the raw address to send: LBA for SDHC, byte address for SDSC.
 */
static int sd_read_sector(uint32_t addr, uint8_t *buf)
{
    sd_cs_low();

    uint8_t r1 = sd_send_cmd(CMD17, addr);
    if (r1 != 0x00u) {
        sd_cs_high();
        return -EIO;
    }

    /* Wait for data token */
    for (int i = 0; i < SD_DATA_TIMEOUT; i++) {
        uint8_t tok = spi_xfer(0xFFu);
        if (tok == SD_TOKEN_DATA)
            goto got_token;
        if (tok != 0xFFu) {
            sd_cs_high();
            return -EIO;    /* error token */
        }
    }
    sd_cs_high();
    return -ETIMEDOUT;

got_token:
    spi_xfer_block(NULL, buf, BLKDEV_SECTOR_SIZE);
    /* Discard 2-byte CRC */
    spi_xfer(0xFFu);
    spi_xfer(0xFFu);

    sd_cs_high();
    return 0;
}

/*
 * Write a single 512-byte sector to the SD card.
 */
static int sd_write_sector(uint32_t addr, const uint8_t *buf)
{
    sd_cs_low();

    uint8_t r1 = sd_send_cmd(CMD24, addr);
    if (r1 != 0x00u) {
        sd_cs_high();
        return -EIO;
    }

    /* Send data token */
    spi_xfer(SD_TOKEN_DATA);
    /* Send sector data */
    spi_xfer_block(buf, NULL, BLKDEV_SECTOR_SIZE);
    /* Send dummy CRC */
    spi_xfer(0xFFu);
    spi_xfer(0xFFu);

    /* Check data response: xxx0sss1, sss should be 010 (accepted) */
    uint8_t resp = spi_xfer(0xFFu);
    if ((resp & SD_DRESP_MASK) != SD_DRESP_ACCEPTED) {
        sd_cs_high();
        return -EIO;
    }

    /* Wait for write completion (busy = MISO low) */
    for (int i = 0; i < SD_WRITE_TIMEOUT; i++) {
        if (spi_xfer(0xFFu) != 0x00u)
            break;
    }

    sd_cs_high();
    return 0;
}

/* ── blkdev read/write wrappers ──────────────────────────────────────────── */

static int sd_blk_read(struct blkdev *dev, void *buf,
                       uint32_t sector, uint32_t count)
{
    (void)dev;
    uint8_t *p = (uint8_t *)buf;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t raw_sector = sd_part_lba + sector + i;
        uint32_t addr = sd_sdhc ? raw_sector : raw_sector * BLKDEV_SECTOR_SIZE;
        int rc = sd_read_sector(addr, p);
        if (rc < 0)
            return rc;
        p += BLKDEV_SECTOR_SIZE;
    }
    return 0;
}

static int sd_blk_write(struct blkdev *dev, const void *buf,
                        uint32_t sector, uint32_t count)
{
    (void)dev;
    const uint8_t *p = (const uint8_t *)buf;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t raw_sector = sd_part_lba + sector + i;
        uint32_t addr = sd_sdhc ? raw_sector : raw_sector * BLKDEV_SECTOR_SIZE;
        int rc = sd_write_sector(addr, p);
        if (rc < 0)
            return rc;
        p += BLKDEV_SECTOR_SIZE;
    }
    return 0;
}

/* ── MBR partition parsing ───────────────────────────────────────────────── */

/*
 * Read sector 0 (MBR), find the first FAT32 partition (type 0x0B or 0x0C).
 * Sets sd_part_lba and sd_part_sectors on success.
 * Returns 0 on success, negative errno on failure.
 */
static int sd_parse_mbr(void)
{
    uint8_t mbr[BLKDEV_SECTOR_SIZE];
    uint32_t addr = sd_sdhc ? 0 : 0;  /* sector 0 */
    int rc = sd_read_sector(addr, mbr);
    if (rc < 0)
        return rc;

    /* Verify MBR signature */
    if (mbr[510] != MBR_SIG_BYTE0 || mbr[511] != MBR_SIG_BYTE1)
        return -EIO;

    /* Scan 4 partition entries at offsets 0x1BE, 0x1CE, 0x1DE, 0x1EE */
    for (int i = 0; i < 4; i++) {
        uint8_t *ent = &mbr[MBR_PART_OFFSET + i * MBR_PART_ENTRY_SIZE];
        uint8_t type = ent[4];
        if (type == PART_TYPE_FAT32 || type == PART_TYPE_FAT32_LBA) {
            /* FAT32 partition found */
            sd_part_lba = (uint32_t)ent[8]
                        | ((uint32_t)ent[9]  << 8)
                        | ((uint32_t)ent[10] << 16)
                        | ((uint32_t)ent[11] << 24);
            sd_part_sectors = (uint32_t)ent[12]
                            | ((uint32_t)ent[13] << 8)
                            | ((uint32_t)ent[14] << 16)
                            | ((uint32_t)ent[15] << 24);
            return 0;
        }
    }

    /* No FAT32 partition: treat entire disk as a single partition
     * (common for small images / SD cards without MBR) */
    sd_part_lba = 0;
    sd_part_sectors = 0;  /* unknown — will be set from card capacity */
    return 0;
}

/* ── Public init ────────────────────────────────────────────────────────── */

int sd_init(void)
{
    /* 1. Check card presence */
    if (!spi_card_detect())
        return -ENODEV;

    /* 2. Power-up delay + ≥74 clock pulses with CS high */
    delay_ms(2);
    sd_cs_high();   /* sets CS high */
    for (int i = 0; i < 10; i++)
        spi_xfer(0xFFu);

    /* 3. CMD0 — GO_IDLE_STATE */
    sd_cs_low();
    uint8_t r1 = sd_send_cmd(CMD0, 0);
    sd_cs_high();
    if (r1 != R1_IDLE)
        return -ETIMEDOUT;

    /* 4. CMD8 — SEND_IF_COND (SD v2.0 check) */
    sd_cs_low();
    r1 = sd_send_cmd(CMD8, CMD8_CHECK_PATTERN);
    if (r1 == R1_IDLE) {
        /* SD v2.0+: read R7 trailing 4 bytes */
        uint8_t r7[4];
        spi_xfer_block(NULL, r7, 4);
        sd_cs_high();
        if (r7[2] != 0x01u || r7[3] != 0xAAu)
            return -EIO;   /* voltage/pattern mismatch */
    } else {
        sd_cs_high();
        /* SD v1.x or MMC — no SDHC support */
    }

    /* 5. ACMD41 — SD_SEND_OP_COND (HCS=1 for SDHC support) */
    {
        int attempts = 1000;
        while (attempts-- > 0) {
            sd_cs_low();
            r1 = sd_send_acmd(ACMD41, ACMD41_HCS);
            sd_cs_high();
            if (r1 == 0x00u)
                break;
            if (r1 != R1_IDLE)
                return -EIO;
            delay_ms(1);
        }
        if (r1 != 0x00u)
            return -ETIMEDOUT;
    }

    /* 6. CMD58 — READ_OCR (check CCS bit for SDHC) */
    sd_cs_low();
    r1 = sd_send_cmd(CMD58, 0);
    if (r1 == 0x00u) {
        uint8_t ocr[4];
        spi_xfer_block(NULL, ocr, 4);
        sd_sdhc = (ocr[0] & OCR_CCS) ? 1 : 0;
    } else {
        sd_sdhc = 0;
    }
    sd_cs_high();

    /* 7. For SDSC: set block length to 512 */
    if (!sd_sdhc) {
        sd_cs_low();
        sd_send_cmd(CMD16, BLKDEV_SECTOR_SIZE);
        sd_cs_high();
    }

    /* 8. Raise SPI to 25 MHz for normal operation */
    spi_set_baud(SD_SPI_FAST_HZ);

    /* 9. Parse MBR to find FAT32 partition */
    int rc = sd_parse_mbr();
    if (rc < 0)
        return rc;

    /* 10. Register as "mmcblk0" block device */
    blkdev_t dev = {
        .name         = "mmcblk0",
        .sector_count = sd_part_sectors,
        .priv         = NULL,
        .read         = sd_blk_read,
        .write        = sd_blk_write,
    };
    rc = blkdev_register(&dev);
    if (rc < 0)
        return rc;

    return 0;
}
