/*
 * rtc_cmos.h — read the MC146818-compatible CMOS RTC on PC/XT-class
 *              hardware (or any emulator that implements it).
 *
 * The CMOS device exposes two I/O ports:
 *
 *   0x70 — index register (write-only; bit 7 controls the NMI mask,
 *          which we keep set while probing to avoid side effects).
 *   0x71 — data register (read/write the byte at the selected index).
 *
 * This driver only reads; CMOS writeback (e.g. for `date -s` to
 * persist across reboot) is explicitly out of scope — see the
 * file_timestamps proposal for the "who owns the clock" discussion
 * that defers it.
 */

#ifndef PPAP_TARGET_PCXT_KERNEL_CORE_DRIVER_RTC_CMOS_H
#define PPAP_TARGET_PCXT_KERNEL_CORE_DRIVER_RTC_CMOS_H

#include <stdint.h>

/* Read the CMOS real-time clock and return seconds since the Unix
 * epoch (UTC — PPAP has no timezone layer, CMOS is treated as UTC).
 *
 * Returns 0 on success and writes the epoch value to *out.  Returns
 * a negative value on failure; *out is left untouched.  Failure
 * modes: UIP flag never clears, read-twice-compare never stabilises,
 * or the decoded Y/M/D/h/m/s falls outside [1970..2106] or other
 * obviously insane ranges. */
int cmos_rtc_read_epoch(uint32_t *out);

#endif /* PPAP_TARGET_PCXT_KERNEL_CORE_DRIVER_RTC_CMOS_H */
