/*
 * sd.h — SD card driver interface (SPI mode)
 *
 * Implements the SD SPI-mode protocol: CMD0 → CMD8 → ACMD41 → CMD58
 * initialisation, CMD17/CMD24 single-block read/write, and MBR parsing
 * to locate the first FAT32 partition.
 *
 * On success, sd_init() registers an "mmcblk0" block device via the
 * blkdev layer.  If no card is present, sd_init() returns -ENODEV and
 * the system falls back to romfs only.
 */

#ifndef PPAP_DRIVERS_SD_H
#define PPAP_DRIVERS_SD_H

#include <stdint.h>

/*
 * Initialise the SD card and register as "mmcblk0" block device.
 *
 * Steps:
 *   1. Check card-detect pin (GP22).  If no card → return -ENODEV.
 *   2. Send ≥74 clock pulses with CS high.
 *   3. CMD0 → idle, CMD8 → v2.0, ACMD41 → ready, CMD58 → CCS.
 *   4. Raise SPI to 25 MHz.
 *   5. Read sector 0 (MBR), locate first FAT32 partition.
 *   6. Register block device with partition LBA offset and sector count.
 *
 * Returns 0 on success, negative errno on failure:
 *   -ENODEV  No card detected
 *   -ETIMEDOUT  Card did not respond
 *   -EIO     Protocol error (bad response, CRC, etc.)
 */
int sd_init(void);

#endif /* PPAP_DRIVERS_SD_H */
