/*
 * rtc_cmos.c — MC146818-compatible CMOS real-time clock reader.
 *
 * See rtc_cmos.h for the API contract and port layout.  This file
 * decodes the Y/M/D/h/m/s fields (BCD or binary, 12h or 24h) and
 * converts to Unix epoch seconds using a proleptic Gregorian
 * calendar, no DST, no timezone.
 */

#include "kernel/core/driver/rtc_cmos.h"

#include "kernel/common/ioregs.h"
#include "kernel/common/irq.h"

#define CMOS_INDEX 0x70u
#define CMOS_DATA  0x71u

/* CMOS register indices */
#define CMOS_REG_SEC      0x00u
#define CMOS_REG_MIN      0x02u
#define CMOS_REG_HOUR     0x04u
#define CMOS_REG_MDAY     0x07u
#define CMOS_REG_MONTH    0x08u
#define CMOS_REG_YEAR     0x09u
#define CMOS_REG_STATUS_B 0x0Bu
#define CMOS_REG_CENTURY  0x32u

/* Status B bits */
#define STATUS_B_BINARY  0x04u /* 1 = binary mode, 0 = BCD */
#define STATUS_B_24HOUR  0x02u /* 1 = 24-hour, 0 = 12-hour + PM bit */
#define HOUR_PM_BIT      0x80u /* 12-hour mode PM marker */

/* Bit 7 of the index register masks NMI; keep it set on every write
 * so we don't accidentally enable/disable NMI as a side effect. */
#define NMI_DISABLE 0x80u

static uint8_t bcd_to_bin(uint8_t v) {
  return (uint8_t)(((v >> 4) & 0x0Fu) * 10u + (v & 0x0Fu));
}

static int is_leap(int year) {
  return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static uint32_t gregorian_to_epoch(int year, int mon, int mday, int hour,
                                   int min, int sec) {
  /* Cumulative days before month start, non-leap year.  mon is 1-based. */
  static const uint16_t ycumdays[12] = {0,   31,  59,  90,  120, 151,
                                        181, 212, 243, 273, 304, 334};

  uint32_t days = 0;
  for (int y = 1970; y < year; y++) days += is_leap(y) ? 366u : 365u;

  days += ycumdays[mon - 1];
  if (mon > 2 && is_leap(year)) days++;

  days += (uint32_t)(mday - 1);

  return days * 86400u + (uint32_t)hour * 3600u + (uint32_t)min * 60u +
         (uint32_t)sec;
}

int cmos_rtc_read_epoch(uint32_t *out) {
  /* Read-twice-compare handles the UIP window naturally: if any byte
   * changed between the two samples the update was in progress, so we
   * retry.  No explicit UIP poll — in practice QEMU's RTC reports UIP
   * clear faster than we can observe it, and real hardware still needs
   * the stability check anyway.
   *
   * The century register (0x32) is deliberately excluded from the
   * compare: on QEMU -machine pc it isn't wired up, so it returns
   * arbitrary bus data that churns between reads and would defeat the
   * stability check. */
  uint8_t sec0, min0, hr0, day0, mon0, yr0;
  uint8_t sec1, min1, hr1, day1, mon1, yr1;
  uint8_t cent1;
  uint8_t statusB;
  int tries;
  for (tries = 0; tries < 8; tries++) {
    /* Disable interrupts for the entire two-sweep + compare so the
     * BIOS INT 08h tick handler can't poke CMOS (ports 0x70/0x71)
     * mid-read, which manifests on QEMU as register 0 reading 0 the
     * first time it's accessed after the BIOS has touched the chip. */
    uint32_t irq_flags = arch_irq_save();

    outb(CMOS_INDEX, NMI_DISABLE | CMOS_REG_SEC);   sec0 = inb(CMOS_DATA);
    outb(CMOS_INDEX, NMI_DISABLE | CMOS_REG_MIN);   min0 = inb(CMOS_DATA);
    outb(CMOS_INDEX, NMI_DISABLE | CMOS_REG_HOUR);  hr0  = inb(CMOS_DATA);
    outb(CMOS_INDEX, NMI_DISABLE | CMOS_REG_MDAY);  day0 = inb(CMOS_DATA);
    outb(CMOS_INDEX, NMI_DISABLE | CMOS_REG_MONTH); mon0 = inb(CMOS_DATA);
    outb(CMOS_INDEX, NMI_DISABLE | CMOS_REG_YEAR);  yr0  = inb(CMOS_DATA);

    outb(CMOS_INDEX, NMI_DISABLE | CMOS_REG_SEC);   sec1 = inb(CMOS_DATA);
    outb(CMOS_INDEX, NMI_DISABLE | CMOS_REG_MIN);   min1 = inb(CMOS_DATA);
    outb(CMOS_INDEX, NMI_DISABLE | CMOS_REG_HOUR);  hr1  = inb(CMOS_DATA);
    outb(CMOS_INDEX, NMI_DISABLE | CMOS_REG_MDAY);  day1 = inb(CMOS_DATA);
    outb(CMOS_INDEX, NMI_DISABLE | CMOS_REG_MONTH); mon1 = inb(CMOS_DATA);
    outb(CMOS_INDEX, NMI_DISABLE | CMOS_REG_YEAR);  yr1  = inb(CMOS_DATA);

    arch_irq_restore(irq_flags);
    if (sec0 == sec1 && min0 == min1 && hr0 == hr1 && day0 == day1 &&
        mon0 == mon1 && yr0 == yr1) break;
  }
  if (tries == 8) return -1;
  outb(CMOS_INDEX, NMI_DISABLE | CMOS_REG_CENTURY);  cent1 = inb(CMOS_DATA);
  outb(CMOS_INDEX, NMI_DISABLE | CMOS_REG_STATUS_B); statusB = inb(CMOS_DATA);
  int is_bcd = !(statusB & STATUS_B_BINARY);
  int is_24h = (statusB & STATUS_B_24HOUR) != 0u;

  /* The PM bit in 12-hour mode lives on top of the hour byte; strip
   * it out of the value before BCD decoding, remember it for later. */
  uint8_t hr_raw = hr1;
  int pm = 0;
  if (!is_24h) {
    pm = (hr_raw & HOUR_PM_BIT) != 0;
    hr_raw = (uint8_t)(hr_raw & (uint8_t)~HOUR_PM_BIT);
  }

  int sec_v  = is_bcd ? bcd_to_bin(sec1)  : (int)sec1;
  int min_v  = is_bcd ? bcd_to_bin(min1)  : (int)min1;
  int hr_v   = is_bcd ? bcd_to_bin(hr_raw): (int)hr_raw;
  int day_v  = is_bcd ? bcd_to_bin(day1)  : (int)day1;
  int mon_v  = is_bcd ? bcd_to_bin(mon1)  : (int)mon1;
  int yr_v   = is_bcd ? bcd_to_bin(yr1)   : (int)yr1;
  int cent_v = is_bcd ? bcd_to_bin(cent1) : (int)cent1;

  /* 12-hour mode: 12 PM → 12, 1..11 PM → 13..23, 12 AM → 0, 1..11 AM → 1..11 */
  if (!is_24h) {
    if (hr_v == 12) hr_v = 0;
    if (pm) hr_v += 12;
  }

  /* Century: modern emulators (QEMU, 86Box, PCem) report 20 here.
   * Some real-iron BIOSes leave it 0.  Fall back to a sane guess. */
  int year;
  if (cent_v >= 19 && cent_v <= 21) {
    year = cent_v * 100 + yr_v;
  } else {
    year = (yr_v < 80) ? 2000 + yr_v : 1900 + yr_v;
  }

  if (mon_v < 1 || mon_v > 12) return -1;
  if (day_v < 1 || day_v > 31) return -1;
  if (hr_v < 0 || hr_v > 23) return -1;
  if (min_v < 0 || min_v > 59) return -1;
  if (sec_v < 0 || sec_v > 59) return -1;
  if (year < 1970 || year > 2106) return -1;

  *out = gregorian_to_epoch(year, mon_v, day_v, hr_v, min_v, sec_v);
  return 0;
}
