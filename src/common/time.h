/*
 * common/time.h — Shared time types for PPAP syscalls
 *
 * struct timespec is passed by nanosleep, clock_gettime32,
 * clock_nanosleep32, ppoll, and any other 32-bit time syscall.
 * Layout matches the Linux 32-bit timespec on ARM / m68k / RISC-V.
 */

#ifndef PPAP_COMMON_TIME_H
#define PPAP_COMMON_TIME_H

struct timespec {
  long tv_sec;  /* seconds                      */
  long tv_nsec; /* nanoseconds [0, 999999999]   */
};

#endif /* PPAP_COMMON_TIME_H */
